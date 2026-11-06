/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A native LibGUI "Notepad": the classic Notepad feature set drawn with the
// port's own Win2000-style theme instead of the Qt/Windows-11 look. It replaces
// the GTK mousepad shim that used to sit behind /bin/TextEditor and crashed easily.
//
// Feature set:
//  - File: New / Open / Save / Save As, opens the file passed on the command
//    line (double-clicking a .txt in the file manager launches us with the
//    file as argument).
//  - Encodings: UTF-8, UTF-8 with BOM, UTF-16 LE/BE and GB18030. Auto-detected
//    on open (BOM -> strict UTF-8 validation -> GB18030 fallback), selectable
//    for the next save, plus "Reopen as" to re-decode the current file.
//  - Line endings: LF / CRLF / CR, detected on open, convertible from the
//    Format menu; the file is written back with the chosen line ending.
//  - Syntax highlighting: auto-detected from the file extension (Markdown,
//    C++, CMake, JavaScript, INI, Shell, GML, Git commit), overridable from
//    the Format > Syntax menu.
//  - Word wrap (on by default, persisted), font picker, F5 inserts the current
//    date and time, Ctrl+F incremental find, F3 / Shift+F3 find next / previous,
//    Ctrl+H find and replace, Ctrl+L go to line/column.
//  - Unsaved changes are confirmed before discarding (New / Open / Exit /
//    window close); the title bar shows a live "*" while the document is
//    modified; the status bar shows line/column, encoding, line ending and
//    language.

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibCMake/CMakeCache/SyntaxHighlighter.h>
#include <LibCMake/SyntaxHighlighter.h>
#include <LibConfig/Client.h>
#include <LibCore/DateTime.h>
#include <LibCore/File.h>
#include <LibCpp/SyntaxHighlighter.h>
#include <LibFileSystemAccessClient/Client.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CheckBox.h>
#include <LibGUI/Event.h>
#include <LibGUI/FontPicker.h>
#include <LibGUI/GML/SyntaxHighlighter.h>
#include <LibGUI/Icon.h>
#include <LibGUI/GitCommitSyntaxHighlighter.h>
#include <LibGUI/INISyntaxHighlighter.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Statusbar.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/TextPosition.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibJS/SyntaxHighlighter.h>
#include <LibMain/Main.h>
#include <LibMarkdown/SyntaxHighlighter.h>
#include <LibShell/SyntaxHighlighter.h>
#include <LibSyntax/Highlighter.h>
#include <LibSyntax/Language.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>

namespace {

enum class Encoding {
    Utf8,
    Utf8Bom,
    Utf16Le,
    Utf16Be,
    Gb18030,
};

enum class LineEnding {
    Lf,
    Crlf,
    Cr,
};

struct DocumentFormat {
    Encoding encoding { Encoding::Utf8 };
    LineEnding line_ending { LineEnding::Lf };
};

struct DecodedDocument {
    DocumentFormat format {};
    // Valid UTF-8, with newlines normalized to '\n'.
    ByteString text;
};

// Display name used in the status bar and the Encoding menu.
StringView encoding_label(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8:
        return "UTF-8"sv;
    case Encoding::Utf8Bom:
        return "UTF-8 BOM"sv;
    case Encoding::Utf16Le:
        return "UTF-16 LE"sv;
    case Encoding::Utf16Be:
        return "UTF-16 BE"sv;
    case Encoding::Gb18030:
        return "GB18030"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView line_ending_label(LineEnding ending)
{
    switch (ending) {
    case LineEnding::Lf:
        return "LF"sv;
    case LineEnding::Crlf:
        return "CRLF"sv;
    case LineEnding::Cr:
        return "CR"sv;
    }
    VERIFY_NOT_REACHED();
}

LineEnding detect_line_ending(StringView text)
{
    if (text.find("\r\n"sv).has_value())
        return LineEnding::Crlf;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '\r')
            return LineEnding::Cr;
    }
    return LineEnding::Lf;
}

ByteString normalize_newlines(ByteString const& text)
{
    return text.replace("\r\n"sv, "\n"sv, AK::ReplaceMode::All).replace("\r"sv, "\n"sv, AK::ReplaceMode::All);
}

ErrorOr<void> encode_with(TextCodec::Encoder& encoder, ByteString const& text, ByteBuffer& out)
{
    Utf8View view { text };
    return encoder.process(view,
        [&](u8 byte) -> ErrorOr<void> {
            out.append(byte);
            return {};
        },
        [&](u32) -> ErrorOr<void> {
            return Error::from_string_view("a character cannot be represented in this encoding"sv);
        });
}

// Encode the editor's text (UTF-8, newlines as '\n') into the target format for writing to disk.
ErrorOr<ByteBuffer> encode_document(ByteString const& editor_text, DocumentFormat format)
{
    // 1. Convert line endings.
    ByteString with_endings;
    switch (format.line_ending) {
    case LineEnding::Lf:
        with_endings = editor_text;
        break;
    case LineEnding::Crlf:
        with_endings = editor_text.replace("\n"sv, "\r\n"sv, AK::ReplaceMode::All);
        break;
    case LineEnding::Cr:
        with_endings = editor_text.replace("\n"sv, "\r"sv, AK::ReplaceMode::All);
        break;
    }

    // 2. Encode to the target encoding, prepending a BOM where appropriate.
    ByteBuffer out;
    switch (format.encoding) {
    case Encoding::Utf8:
        out.append(with_endings.bytes());
        return out;
    case Encoding::Utf8Bom: {
        static constexpr u8 bom[] { 0xEF, 0xBB, 0xBF };
        out.append(bom, sizeof(bom));
        out.append(with_endings.bytes());
        return out;
    }
    case Encoding::Utf16Le:
    case Encoding::Utf16Be: {
        static constexpr u8 le_bom[] { 0xFF, 0xFE };
        static constexpr u8 be_bom[] { 0xFE, 0xFF };
        if (format.encoding == Encoding::Utf16Le)
            out.append(le_bom, sizeof(le_bom));
        else
            out.append(be_bom, sizeof(be_bom));
        auto name = format.encoding == Encoding::Utf16Le ? "utf-16le"sv : "utf-16be"sv;
        auto encoder = TextCodec::encoder_for_exact_name(name);
        if (!encoder.has_value())
            return Error::from_string_view("UTF-16 encoder not available"sv);
        TRY(encode_with(encoder.value(), with_endings, out));
        return out;
    }
    case Encoding::Gb18030: {
        auto encoder = TextCodec::encoder_for_exact_name("gb18030"sv);
        if (!encoder.has_value())
            return Error::from_string_view("GB18030 encoder not available"sv);
        TRY(encode_with(encoder.value(), with_endings, out));
        return out;
    }
    }
    VERIFY_NOT_REACHED();
}

// Decode raw file bytes into the document format and normalized UTF-8 text.
// If `forced_encoding` is set, skip detection and decode with that encoding.
Optional<DecodedDocument> decode_document(ByteBuffer const& bytes, Optional<Encoding> forced_encoding)
{
    StringView view { bytes.bytes() };
    auto const* raw = bytes.data();

    TextCodec::Decoder* decoder = nullptr;
    Encoding encoding = Encoding::Utf8;
    size_t offset = 0;

    if (forced_encoding.has_value()) {
        switch (*forced_encoding) {
        case Encoding::Utf8:
        case Encoding::Utf8Bom: {
            auto utf8 = TextCodec::decoder_for_exact_name("utf-8"sv);
            if (!utf8.has_value())
                return {};
            decoder = &utf8.value();
            encoding = *forced_encoding;
            // Strip a leading BOM if present so re-saving doesn't accumulate them.
            if (bytes.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
                offset = 3;
            break;
        }
        case Encoding::Utf16Le: {
            auto le = TextCodec::decoder_for_exact_name("utf-16le"sv);
            if (!le.has_value())
                return {};
            decoder = &le.value();
            encoding = Encoding::Utf16Le;
            if (bytes.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
                offset = 2;
            break;
        }
        case Encoding::Utf16Be: {
            auto be = TextCodec::decoder_for_exact_name("utf-16be"sv);
            if (!be.has_value())
                return {};
            decoder = &be.value();
            encoding = Encoding::Utf16Be;
            if (bytes.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF)
                offset = 2;
            break;
        }
        case Encoding::Gb18030: {
            auto gb18030 = TextCodec::decoder_for_exact_name("gb18030"sv);
            if (!gb18030.has_value())
                return {};
            decoder = &gb18030.value();
            encoding = Encoding::Gb18030;
            break;
        }
        }
    } else if (auto bom = TextCodec::bom_sniff_to_decoder(view); bom.has_value()) {
        auto& bom_decoder = bom.value();
        auto utf8 = TextCodec::decoder_for_exact_name("utf-8"sv);
        auto le = TextCodec::decoder_for_exact_name("utf-16le"sv);
        auto be = TextCodec::decoder_for_exact_name("utf-16be"sv);
        if (utf8.has_value() && &bom_decoder == &utf8.value()) {
            encoding = Encoding::Utf8Bom;
            offset = 3;
        } else if (le.has_value() && &bom_decoder == &le.value()) {
            encoding = Encoding::Utf16Le;
            offset = 2;
        } else if (be.has_value() && &bom_decoder == &be.value()) {
            encoding = Encoding::Utf16Be;
            offset = 2;
        }
        decoder = &bom_decoder;
    } else {
        auto utf8 = TextCodec::decoder_for_exact_name("utf-8"sv);
        if (utf8.has_value() && utf8.value().validate(view)) {
            decoder = &utf8.value();
            encoding = Encoding::Utf8;
        } else {
            auto gb18030 = TextCodec::decoder_for_exact_name("gb18030"sv);
            if (!gb18030.has_value())
                return {};
            decoder = &gb18030.value();
            encoding = Encoding::Gb18030;
        }
    }

    auto decoded = decoder->to_utf8(view.substring_view(offset));
    if (decoded.is_error())
        return {};
    auto raw_text = decoded.value().to_byte_string();
    auto line_ending = detect_line_ending(raw_text);
    return DecodedDocument {
        { encoding, line_ending },
        normalize_newlines(raw_text),
    };
}

constexpr Encoding all_encodings[] {
    Encoding::Utf8,
    Encoding::Utf8Bom,
    Encoding::Utf16Le,
    Encoding::Utf16Be,
    Encoding::Gb18030,
};

// ---------------------------------------------------------------------------
// Lightweight syntax highlighters (no LibWeb / LibSQL dependency).
// Each is a small hand-written lexer over LibSyntax::Highlighter.
// ---------------------------------------------------------------------------

namespace {

static Syntax::TextPosition position_at(StringView const& text, size_t byte_offset)
{
    size_t line = 0;
    size_t column = 0;
    for (size_t j = 0; j < byte_offset && j < text.length(); ++j) {
        if (text[j] == '\n') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }
    return Syntax::TextPosition(line, column);
}

static Gfx::TextAttributes style_keyword(Gfx::Palette const& palette) { return { palette.syntax_keyword(), {}, true }; }
static Gfx::TextAttributes style_identifier(Gfx::Palette const& palette) { return { palette.syntax_identifier() }; }
static Gfx::TextAttributes style_string(Gfx::Palette const& palette) { return { palette.syntax_string() }; }
static Gfx::TextAttributes style_comment(Gfx::Palette const& palette) { return { palette.syntax_comment() }; }
static Gfx::TextAttributes style_punctuation(Gfx::Palette const& palette) { return { palette.syntax_punctuation(), {}, true }; }

// ---------------- HTML ----------------

class HtmlSyntaxHighlighter final : public Syntax::Highlighter {
public:
    enum class TokenType {
        Text,
        TagOpen,
        TagClose,
        TagName,
        AttributeName,
        AttributeValue,
        Comment,
    };

    virtual Syntax::Language language() const override { return Syntax::Language::HTML; }
    virtual Optional<StringView> comment_prefix() const override { return "<!--"sv; }
    virtual Optional<StringView> comment_suffix() const override { return "-->"sv; }

    virtual void rehighlight(Palette const& palette) override
    {
        auto text = m_client->get_text();
        Vector<Syntax::TextDocumentSpan> spans;

        size_t i = 0;

        auto emit = [&](size_t start, size_t end, TokenType type) {
            if (end <= start)
                return;
            Syntax::TextDocumentSpan span;
            span.range.set_start(position_at(text, start));
            span.range.set_end(position_at(text, end));
            switch (type) {
            case TokenType::TagOpen:
            case TokenType::TagClose:
                span.attributes = style_punctuation(palette);
                break;
            case TokenType::TagName:
                span.attributes = style_keyword(palette);
                break;
            case TokenType::AttributeName:
                span.attributes = style_identifier(palette);
                break;
            case TokenType::AttributeValue:
                span.attributes = style_string(palette);
                break;
            case TokenType::Comment:
                span.attributes = style_comment(palette);
                break;
            default:
                span.attributes = { palette.base_text() };
                break;
            }
            span.data = static_cast<u64>(type);
            spans.append(span);
        };

        while (i < text.length()) {
            // Comment: <!-- ... -->
            if (text.substring_view(i).starts_with("<!--"sv)) {
                auto end = text.find("-->"sv, i);
                if (!end.has_value()) {
                    emit(i, text.length(), TokenType::Comment);
                    break;
                }
                emit(i, *end + 3, TokenType::Comment);
                i = *end + 3;
                continue;
            }

            // Tag: <...>
            if (text[i] == '<') {
                size_t tag_start = i;
                ++i; // skip '<'
                emit(tag_start, i, TokenType::TagOpen);

                // Optional closing slash
                if (i < text.length() && text[i] == '/') {
                    ++i;
                }

                // Tag name
                size_t name_start = i;
                while (i < text.length() && is_ascii_alphanumeric(text[i]))
                    ++i;
                if (i > name_start)
                    emit(name_start, i, TokenType::TagName);

                // Whitespace + attributes
                while (i < text.length() && text[i] != '>' && text[i] != '/') {
                    // skip whitespace
                    while (i < text.length() && is_ascii_space(text[i]))
                        ++i;
                    if (i < text.length() && text[i] == '>')
                        break;

                    // attribute name
                    size_t attr_start = i;
                    while (i < text.length() && (is_ascii_alphanumeric(text[i]) || text[i] == '-' || text[i] == '_'))
                        ++i;
                    if (i > attr_start)
                        emit(attr_start, i, TokenType::AttributeName);
                    else
                        ++i; // ensure progress on non-identifier bytes (e.g. '!', '/')

                    // skip whitespace
                    while (i < text.length() && is_ascii_space(text[i]))
                        ++i;

                    // attribute value
                    if (i < text.length() && text[i] == '=') {
                        ++i;
                        while (i < text.length() && is_ascii_space(text[i]))
                            ++i;
                        if (i < text.length() && (text[i] == '"' || text[i] == '\'')) {
                            char quote = text[i];
                            ++i;
                            size_t val_start = i;
                            while (i < text.length() && text[i] != quote)
                                ++i;
                            if (i < text.length())
                                ++i; // skip closing quote
                            emit(val_start, i, TokenType::AttributeValue);
                        }
                    }
                }

                if (i < text.length() && text[i] == '>') {
                    emit(i, i + 1, TokenType::TagClose);
                    ++i;
                }
                continue;
            }

            // Plain text
            size_t text_start = i;
            while (i < text.length() && text[i] != '<')
                ++i;
            emit(text_start, i, TokenType::Text);
        }

        m_client->do_set_spans(move(spans));
        m_client->do_update();
    }

protected:
    virtual Vector<MatchingTokenPair> matching_token_pairs_impl() const override { return {}; }
    virtual bool token_types_equal(u64, u64) const override { return true; }
};

// ---------------- CSS ----------------

class CssSyntaxHighlighter final : public Syntax::Highlighter {
public:
    enum class TokenType {
        Text,
        Selector,
        Property,
        Value,
        Comment,
        Punctuation,
    };

    virtual Syntax::Language language() const override { return Syntax::Language::CSS; }
    virtual Optional<StringView> comment_prefix() const override { return "/*"sv; }
    virtual Optional<StringView> comment_suffix() const override { return "*/"sv; }

    virtual void rehighlight(Palette const& palette) override
    {
        auto text = m_client->get_text();
        Vector<Syntax::TextDocumentSpan> spans;

        size_t i = 0;
        auto emit = [&](size_t start, size_t end, TokenType type) {
            if (end <= start)
                return;
            Syntax::TextDocumentSpan span;
            span.range.set_start(position_at(text, start));
            span.range.set_end(position_at(text, end));
            switch (type) {
            case TokenType::Selector:
                span.attributes = style_keyword(palette);
                break;
            case TokenType::Property:
                span.attributes = style_identifier(palette);
                break;
            case TokenType::Value:
                span.attributes = style_string(palette);
                break;
            case TokenType::Comment:
                span.attributes = style_comment(palette);
                break;
            case TokenType::Punctuation:
                span.attributes = style_punctuation(palette);
                break;
            default:
                span.attributes = { palette.base_text() };
                break;
            }
            span.data = static_cast<u64>(type);
            spans.append(span);
        };

        while (i < text.length()) {
            // Comment: /* ... */
            if (i + 1 < text.length() && text[i] == '/' && text[i + 1] == '*') {
                auto end = text.find("*/"sv, i + 2);
                if (!end.has_value()) {
                    emit(i, text.length(), TokenType::Comment);
                    break;
                }
                emit(i, *end + 2, TokenType::Comment);
                i = *end + 2;
                continue;
            }

            // Punctuation
            if (text[i] == '{' || text[i] == '}' || text[i] == ':' || text[i] == ';' || text[i] == '(' || text[i] == ')') {
                emit(i, i + 1, TokenType::Punctuation);
                ++i;
                continue;
            }

            // Whitespace
            if (is_ascii_space(text[i])) {
                ++i;
                continue;
            }

            // Scan a token run (selector, property, or value)
            size_t start = i;
            while (i < text.length() && !is_ascii_space(text[i]) && text[i] != '{' && text[i] != '}' && text[i] != ':' && text[i] != ';' && !(i + 1 < text.length() && text[i] == '/' && text[i + 1] == '*'))
                ++i;
            if (i > start)
                emit(start, i, TokenType::Selector);
        }

        m_client->do_set_spans(move(spans));
        m_client->do_update();
    }

protected:
    virtual Vector<MatchingTokenPair> matching_token_pairs_impl() const override { return {}; }
    virtual bool token_types_equal(u64, u64) const override { return true; }
};

// ---------------- SQL ----------------

class SqlSyntaxHighlighter final : public Syntax::Highlighter {
public:
    enum class TokenType {
        Text,
        Keyword,
        Identifier,
        String,
        Number,
        Comment,
        Punctuation,
    };

    static bool is_keyword(StringView word)
    {
        return word.equals_ignoring_ascii_case("SELECT"sv)
            || word.equals_ignoring_ascii_case("FROM"sv)
            || word.equals_ignoring_ascii_case("WHERE"sv)
            || word.equals_ignoring_ascii_case("INSERT"sv)
            || word.equals_ignoring_ascii_case("INTO"sv)
            || word.equals_ignoring_ascii_case("VALUES"sv)
            || word.equals_ignoring_ascii_case("CREATE"sv)
            || word.equals_ignoring_ascii_case("TABLE"sv)
            || word.equals_ignoring_ascii_case("DROP"sv)
            || word.equals_ignoring_ascii_case("DELETE"sv)
            || word.equals_ignoring_ascii_case("UPDATE"sv)
            || word.equals_ignoring_ascii_case("SET"sv)
            || word.equals_ignoring_ascii_case("JOIN"sv)
            || word.equals_ignoring_ascii_case("ON"sv)
            || word.equals_ignoring_ascii_case("AND"sv)
            || word.equals_ignoring_ascii_case("OR"sv)
            || word.equals_ignoring_ascii_case("NOT"sv)
            || word.equals_ignoring_ascii_case("NULL"sv)
            || word.equals_ignoring_ascii_case("DISTINCT"sv)
            || word.equals_ignoring_ascii_case("ORDER"sv)
            || word.equals_ignoring_ascii_case("BY"sv)
            || word.equals_ignoring_ascii_case("GROUP"sv)
            || word.equals_ignoring_ascii_case("HAVING"sv)
            || word.equals_ignoring_ascii_case("LIMIT"sv)
            || word.equals_ignoring_ascii_case("OFFSET"sv)
            || word.equals_ignoring_ascii_case("UNION"sv)
            || word.equals_ignoring_ascii_case("ALL"sv)
            || word.equals_ignoring_ascii_case("AS"sv)
            || word.equals_ignoring_ascii_case("ASC"sv)
            || word.equals_ignoring_ascii_case("DESC"sv);
    }

    virtual Syntax::Language language() const override { return Syntax::Language::SQL; }
    virtual Optional<StringView> comment_prefix() const override { return "--"sv; }
    virtual Optional<StringView> comment_suffix() const override { return {}; }

    virtual void rehighlight(Palette const& palette) override
    {
        auto text = m_client->get_text();
        Vector<Syntax::TextDocumentSpan> spans;

        size_t i = 0;
        auto emit = [&](size_t start, size_t end, TokenType type) {
            if (end <= start)
                return;
            Syntax::TextDocumentSpan span;
            span.range.set_start(position_at(text, start));
            span.range.set_end(position_at(text, end));
            switch (type) {
            case TokenType::Keyword:
                span.attributes = style_keyword(palette);
                break;
            case TokenType::Identifier:
                span.attributes = style_identifier(palette);
                break;
            case TokenType::String:
                span.attributes = style_string(palette);
                break;
            case TokenType::Number:
                span.attributes = { palette.syntax_number() };
                break;
            case TokenType::Comment:
                span.attributes = style_comment(palette);
                break;
            case TokenType::Punctuation:
                span.attributes = style_punctuation(palette);
                break;
            default:
                span.attributes = { palette.base_text() };
                break;
            }
            span.data = static_cast<u64>(type);
            spans.append(span);
        };

        while (i < text.length()) {
            // Line comment: -- ...
            if (i + 1 < text.length() && text[i] == '-' && text[i + 1] == '-') {
                size_t end = i;
                while (end < text.length() && text[end] != '\n')
                    ++end;
                emit(i, end, TokenType::Comment);
                i = end;
                continue;
            }

            // Block comment: /* ... */
            if (i + 1 < text.length() && text[i] == '/' && text[i + 1] == '*') {
                auto end = text.find("*/"sv, i + 2);
                if (!end.has_value()) {
                    emit(i, text.length(), TokenType::Comment);
                    break;
                }
                emit(i, *end + 2, TokenType::Comment);
                i = *end + 2;
                continue;
            }

            // String: '...'
            if (text[i] == '\'') {
                size_t start = i;
                ++i;
                while (i < text.length() && text[i] != '\'') {
                    if (i + 1 < text.length() && text[i] == '\\') ++i; // escaped char
                    ++i;
                }
                if (i < text.length()) ++i; // skip closing quote
                emit(start, i, TokenType::String);
                continue;
            }

            // Number
            if (is_ascii_digit(text[i])) {
                size_t start = i;
                while (i < text.length() && (is_ascii_digit(text[i]) || text[i] == '.'))
                    ++i;
                emit(start, i, TokenType::Number);
                continue;
            }

            // Punctuation
            if (text[i] == '(' || text[i] == ')' || text[i] == ',' || text[i] == ';' || text[i] == '*' || text[i] == '=' || text[i] == '<' || text[i] == '>' || text[i] == '+' || text[i] == '-') {
                emit(i, i + 1, TokenType::Punctuation);
                ++i;
                continue;
            }

            // Whitespace
            if (is_ascii_space(text[i])) {
                ++i;
                continue;
            }

            // Identifier / keyword
            size_t start = i;
            while (i < text.length() && (is_ascii_alphanumeric(text[i]) || text[i] == '_'))
                ++i;
            if (i > start) {
                auto word = text.substring_view(start, i - start);
                if (is_keyword(word))
                    emit(start, i, TokenType::Keyword);
                else
                    emit(start, i, TokenType::Identifier);
            } else {
                ++i; // ensure progress on non-identifier bytes (e.g. '/')
            }
        }

        m_client->do_set_spans(move(spans));
        m_client->do_update();
    }

protected:
    virtual Vector<MatchingTokenPair> matching_token_pairs_impl() const override { return {}; }
    virtual bool token_types_equal(u64, u64) const override { return true; }
};

}

// Syntax highlighting selection in the Format > Syntax menu. Auto means
// "detect from the current file name".
enum class SyntaxChoice {
    Auto,
    PlainText,
    Cpp,
    CMake,
    JavaScript,
    Markdown,
    INI,
    Shell,
    GitCommit,
    GML,
    HTML,
    CSS,
    SQL,
};

Syntax::Language language_for_choice(SyntaxChoice choice)
{
    switch (choice) {
    case SyntaxChoice::Cpp:
        return Syntax::Language::Cpp;
    case SyntaxChoice::CMake:
        return Syntax::Language::CMake;
    case SyntaxChoice::JavaScript:
        return Syntax::Language::JavaScript;
    case SyntaxChoice::Markdown:
        return Syntax::Language::Markdown;
    case SyntaxChoice::INI:
        return Syntax::Language::INI;
    case SyntaxChoice::Shell:
        return Syntax::Language::Shell;
    case SyntaxChoice::GitCommit:
        return Syntax::Language::GitCommit;
    case SyntaxChoice::GML:
        return Syntax::Language::GML;
    case SyntaxChoice::HTML:
        return Syntax::Language::HTML;
    case SyntaxChoice::CSS:
        return Syntax::Language::CSS;
    case SyntaxChoice::SQL:
        return Syntax::Language::SQL;
    case SyntaxChoice::PlainText:
        return Syntax::Language::PlainText;
    case SyntaxChoice::Auto:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

OwnPtr<Syntax::Highlighter> make_highlighter(Syntax::Language language)
{
    switch (language) {
    case Syntax::Language::CMake:
        return make<CMake::SyntaxHighlighter>();
    case Syntax::Language::CMakeCache:
        return make<CMake::Cache::SyntaxHighlighter>();
    case Syntax::Language::Cpp:
        return make<Cpp::SyntaxHighlighter>();
    case Syntax::Language::GitCommit:
        return make<GUI::GitCommitSyntaxHighlighter>();
    case Syntax::Language::GML:
        return make<GUI::GML::SyntaxHighlighter>();
    case Syntax::Language::INI:
        return make<GUI::IniSyntaxHighlighter>();
    case Syntax::Language::JavaScript:
        return make<JS::SyntaxHighlighter>();
    case Syntax::Language::Markdown:
        return make<Markdown::SyntaxHighlighter>();
    case Syntax::Language::Shell:
        return make<Shell::SyntaxHighlighter>();
    case Syntax::Language::HTML:
        return make<HtmlSyntaxHighlighter>();
    case Syntax::Language::CSS:
        return make<CssSyntaxHighlighter>();
    case Syntax::Language::SQL:
        return make<SqlSyntaxHighlighter>();
    default:
        return {};
    }
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::create(arguments));

    auto window = GUI::Window::construct();
    window->set_title("Notepad"sv);
    window->resize(720, 500);
    window->center_on_screen();

    auto app_icon = GUI::Icon::default_icon("app-text-editor"sv);
    window->set_icon(app_icon.bitmap_for_size(16));

    // Root: editor on top, status bar at the bottom.
    auto root = window->set_main_widget<GUI::Widget>();
    root->set_layout<GUI::VerticalBoxLayout>(0, 0);
    root->set_fill_with_background_color(true);
    auto& editor = root->add<GUI::TextEditor>();
    auto& statusbar = root->add<GUI::Statusbar>();
    statusbar.set_segment_count(4);
    statusbar.segment(1).set_mode(GUI::Statusbar::Segment::Mode::Auto);
    statusbar.segment(2).set_mode(GUI::Statusbar::Segment::Mode::Auto);
    statusbar.segment(3).set_mode(GUI::Statusbar::Segment::Mode::Auto);

    // Path of the file currently shown (empty for a new document).
    auto current_path = ByteString {};
    auto current_format = DocumentFormat {};
    auto syntax_choice = SyntaxChoice::Auto;

    struct EncodingAction {
        Encoding encoding;
        NonnullRefPtr<GUI::Action> action;
    };
    struct LineEndingAction {
        LineEnding ending;
        NonnullRefPtr<GUI::Action> action;
    };
    Vector<EncodingAction> encoding_menu_actions;
    Vector<LineEndingAction> line_ending_menu_actions;
    auto sync_format_menus = [&] {
        for (auto& entry : encoding_menu_actions)
            entry.action->set_checked(entry.encoding == current_format.encoding);
        for (auto& entry : line_ending_menu_actions)
            entry.action->set_checked(entry.ending == current_format.line_ending);
    };

    auto update_title = [&] {
        auto name = current_path.is_empty() ? "Untitled"sv : LexicalPath(current_path).basename();
        auto star = editor.document().is_modified() ? " *"sv : ""sv;
        auto title = String::formatted("Notepad  -  {}{}", name, star).release_value_but_fixme_should_propagate_errors();
        window->set_title(title.bytes_as_string_view());
    };

    auto resolved_language = [&]() -> Optional<Syntax::Language> {
        if (syntax_choice != SyntaxChoice::Auto)
            return language_for_choice(syntax_choice);
        if (current_path.is_empty())
            return Syntax::Language::PlainText;
        if (auto language = Syntax::language_from_filename(LexicalPath(current_path)); language.has_value())
            return *language;
        return Syntax::Language::PlainText;
    };

    auto language_label = [&]() -> StringView {
        if (auto language = resolved_language(); language.has_value())
            return Syntax::language_to_string(*language);
        return "Plain Text"sv;
    };

    auto update_statusbar = [&] {
        statusbar.set_text(0, String::formatted("Ln {}, Col {}", editor.cursor().line() + 1, editor.cursor().column()).release_value_but_fixme_should_propagate_errors());
        statusbar.set_text(1, String::from_utf8(encoding_label(current_format.encoding)).release_value_but_fixme_should_propagate_errors());
        statusbar.set_text(2, String::from_utf8(line_ending_label(current_format.line_ending)).release_value_but_fixme_should_propagate_errors());
        statusbar.set_text(3, String::from_utf8(language_label()).release_value_but_fixme_should_propagate_errors());
    };

    auto apply_syntax = [&] {
        auto language = resolved_language();
        if (!language.has_value() || *language == Syntax::Language::PlainText) {
            editor.set_syntax_highlighter({});
        } else {
            editor.set_syntax_highlighter(make_highlighter(*language));
        }
        update_statusbar();
    };

    auto do_save_as = [&]() -> bool {
        // Encode *before* showing the save dialog: on failure nothing is changed on disk.
        auto encoded = encode_document(editor.text(), current_format);
        if (encoded.is_error()) {
            GUI::MessageBox::show(window, "Some characters cannot be saved in this encoding.\n"sv, "Error"sv, GUI::MessageBox::Type::Error);
            return false;
        }
        auto suggested_name = current_path.is_empty() ? "Untitled"sv : LexicalPath(current_path).basename();
        auto suggested_extension = [](StringView path) -> ByteString {
            if (path.is_empty())
                return "txt"sv;
            auto ext = LexicalPath(path).extension();
            return ext.is_empty() ? ByteString { "txt"sv } : ByteString { ext };
        }(current_path);
        auto response = FileSystemAccessClient::Client::the().save_file(window, ByteString { suggested_name }, move(suggested_extension));
        if (response.is_error())
            return false;
        auto file = response.release_value();
        if (file.stream().write_until_depleted(encoded.value().bytes()).is_error()) {
            GUI::MessageBox::show(window, "Unable to save file.\n"sv, "Error"sv, GUI::MessageBox::Type::Error);
            return false;
        }
        current_path = file.filename();
        editor.document().set_unmodified();
        syntax_choice = SyntaxChoice::Auto;
        apply_syntax();
        update_title();
        update_statusbar();
        return true;
    };

    auto do_save = [&]() -> bool {
        if (current_path.is_empty())
            return do_save_as();
        // Encode *before* touching the file: on failure the file on disk is left untouched.
        auto encoded = encode_document(editor.text(), current_format);
        if (encoded.is_error()) {
            GUI::MessageBox::show(window, "Some characters cannot be saved in this encoding.\n"sv, "Error"sv, GUI::MessageBox::Type::Error);
            return false;
        }
        auto response = FileSystemAccessClient::Client::the().request_file(window, current_path, Core::File::OpenMode::Truncate | Core::File::OpenMode::Write);
        if (response.is_error())
            return false;
        if (response.value().stream().write_until_depleted(encoded.value().bytes()).is_error()) {
            GUI::MessageBox::show(window, "Unable to save file.\n"sv, "Error"sv, GUI::MessageBox::Type::Error);
            return false;
        }
        editor.document().set_unmodified();
        update_title();
        update_statusbar();
        return true;
    };

    // Ask before throwing away unsaved changes. Returns true when it's safe to proceed
    // (user saved, or chose to discard). False means "stay in the current document".
    auto confirm_discard = [&]() -> bool {
        if (!editor.document().is_modified())
            return true;
        auto result = GUI::MessageBox::ask_about_unsaved_changes(window, current_path);
        if (result == GUI::Dialog::ExecResult::Yes)
            return do_save();
        return result == GUI::Dialog::ExecResult::No;
    };

    auto do_new = [&] {
        if (!confirm_discard())
            return;
        editor.set_text(""sv);
        current_path = {};
        current_format = {};
        syntax_choice = SyntaxChoice::Auto;
        apply_syntax();
        sync_format_menus();
        editor.document().set_unmodified();
        update_title();
        update_statusbar();
    };

    // Load a stream into the editor, replacing the current document. On failure the current document is left intact.
    auto load_from_stream = [&](Core::File& stream, ByteString const& path, Optional<Encoding> forced_encoding) -> bool {
        auto buffer = stream.read_until_eof();
        if (buffer.is_error()) {
            GUI::MessageBox::show(window, "Unable to open file.\n"sv, "Error"sv, GUI::MessageBox::Type::Error);
            return false;
        }
        auto decoded = decode_document(buffer.value(), forced_encoding);
        if (!decoded.has_value()) {
            GUI::MessageBox::show(window, "Unable to decode file.\n"sv, "Error"sv, GUI::MessageBox::Type::Error);
            return false;
        }
        editor.set_text(decoded->text);
        current_path = path;
        current_format = decoded->format;
        syntax_choice = SyntaxChoice::Auto;
        apply_syntax();
        sync_format_menus();
        editor.document().set_unmodified();
        update_title();
        update_statusbar();
        return true;
    };

    auto do_open = [&] {
        if (!confirm_discard())
            return;
        auto response = FileSystemAccessClient::Client::the().open_file(window);
        if (response.is_error())
            return;
        auto file = response.release_value();
        load_from_stream(file.stream(), file.filename(), {});
    };

    auto reopen_with = [&](Encoding encoding) {
        if (current_path.is_empty()) {
            GUI::MessageBox::show(window, "Save the document to a file first, then reopen it with another encoding.\n"sv, "Notepad"sv, GUI::MessageBox::Type::Information);
            return;
        }
        if (!confirm_discard())
            return;
        auto response = FileSystemAccessClient::Client::the().request_file(window, current_path, Core::File::OpenMode::Read);
        if (response.is_error())
            return;
        load_from_stream(response.value().stream(), current_path, encoding);
    };

    // --- Menu: File ----------------------------------------------------

    auto file_menu = window->add_menu("&File"_string);
    file_menu->add_action(GUI::Action::create("&New"sv, { Mod_Ctrl, Key_N }, [&](auto&) { do_new(); }));
    file_menu->add_action(GUI::Action::create("&Open..."sv, { Mod_Ctrl, Key_O }, [&](auto&) { do_open(); }));
    file_menu->add_action(GUI::Action::create("&Save"sv, { Mod_Ctrl, Key_S }, [&](auto&) { do_save(); }));
    file_menu->add_action(GUI::Action::create("Save &As..."sv, { Mod_Ctrl | Mod_Shift, Key_S }, [&](auto&) { do_save_as(); }));
    file_menu->add_separator();

    GUI::ActionGroup encoding_actions;
    encoding_actions.set_exclusive(true);
    auto encoding_submenu = file_menu->add_submenu("&Encoding"_string);
    for (auto encoding : all_encodings) {
        auto action = GUI::Action::create_checkable(ByteString { encoding_label(encoding) }, [&, encoding](auto&) {
            current_format.encoding = encoding;
            sync_format_menus();
            update_statusbar();
        });
        if (encoding == current_format.encoding)
            action->set_checked(true);
        encoding_actions.add_action(*action);
        encoding_submenu->add_action(*action);
        encoding_menu_actions.append({ encoding, *action });
    }
    encoding_submenu->add_separator();
    encoding_submenu->add_action(GUI::Action::create("&Reopen as UTF-8"sv, [&](auto&) { reopen_with(Encoding::Utf8); }));
    encoding_submenu->add_action(GUI::Action::create("Reopen as UTF-&8 BOM"sv, [&](auto&) { reopen_with(Encoding::Utf8Bom); }));
    encoding_submenu->add_action(GUI::Action::create("Reopen as UTF-16 &LE"sv, [&](auto&) { reopen_with(Encoding::Utf16Le); }));
    encoding_submenu->add_action(GUI::Action::create("Reopen as UTF-16 &BE"sv, [&](auto&) { reopen_with(Encoding::Utf16Be); }));
    encoding_submenu->add_action(GUI::Action::create("Reopen as &GB18030"sv, [&](auto&) { reopen_with(Encoding::Gb18030); }));
    file_menu->add_separator();
    file_menu->add_action(GUI::Action::create("E&xit"sv, { Mod_Alt, Key_F4 }, [&](auto&) {
        if (confirm_discard())
            window->close();
    }));

    // --- Menu: Edit ----------------------------------------------------

    auto edit_menu = window->add_menu("&Edit"_string);
    edit_menu->add_action(editor.undo_action());
    edit_menu->add_action(editor.redo_action());
    edit_menu->add_separator();
    edit_menu->add_action(editor.cut_action());
    edit_menu->add_action(editor.copy_action());
    edit_menu->add_action(editor.paste_action());
    edit_menu->add_separator();
    edit_menu->add_action(GUI::Action::create("&Find..."sv, { Mod_Ctrl, Key_F }, [&](auto&) { editor.show_find_banner(); }));

    // F3 / Shift+F3: find the current selection again (classic "search for selected word"),
    // falling back to nothing when there is no selection.
    auto find_selection = [&](GUI::TextEditor::SearchDirection direction, bool show_message) {
        if (!editor.has_selection()) {
            if (show_message)
                GUI::MessageBox::show(window, "Select some text first, then press F3.\n"sv, "Find"sv, GUI::MessageBox::Type::Information);
            return;
        }
        auto needle = editor.selected_text();
        auto result = editor.find_text(needle, direction, GUI::TextDocument::SearchShouldWrap::Yes, false, false);
        if (!result.is_valid() && show_message)
            GUI::MessageBox::show(window, ByteString::formatted("Not found: {}", needle), "Not found"sv, GUI::MessageBox::Type::Information);
    };
    edit_menu->add_action(GUI::Action::create("Find &Next"sv, Key_F3, [&](auto&) { find_selection(GUI::TextEditor::SearchDirection::Forward, true); }));
    edit_menu->add_action(GUI::Action::create("Find Pre&vious"sv, { Mod_Shift, Key_F3 }, [&](auto&) { find_selection(GUI::TextEditor::SearchDirection::Backward, true); }));

    // Ctrl+H: find and replace, in a modeless window so the document stays visible.
    RefPtr<GUI::CheckBox> search_case, search_wrap;
    RefPtr<GUI::TextBox> find_box, replace_box;
    RefPtr<GUI::Window> replace_window;

    auto show_replace = [&] {
        if (replace_window) {
            replace_window->show();
            replace_window->move_to_front();
            return;
        }
        replace_window = GUI::Window::construct();
        replace_window->set_title("Replace"sv);
        replace_window->set_resizable(false);
        replace_window->set_icon(app_icon.bitmap_for_size(16));

        auto panel = replace_window->set_main_widget<GUI::Widget>();
        panel->set_layout<GUI::VerticalBoxLayout>(6, 6);
        panel->set_fill_with_background_color(true);

        auto& find_row = panel->add<GUI::Widget>();
        find_row.set_layout<GUI::HorizontalBoxLayout>(0, 6);
        auto& find_label = find_row.add<GUI::Label>();
        find_label.set_text("Find what:"_string);
        find_label.set_autosize(true);
        find_box = find_row.add<GUI::TextBox>();
        find_box->set_placeholder("Find"sv);

        auto& replace_row = panel->add<GUI::Widget>();
        replace_row.set_layout<GUI::HorizontalBoxLayout>(0, 6);
        auto& replace_label = replace_row.add<GUI::Label>();
        replace_label.set_text("Replace with:"_string);
        replace_label.set_autosize(true);
        replace_box = replace_row.add<GUI::TextBox>();
        replace_box->set_placeholder("Replace"sv);

        auto& options_row = panel->add<GUI::Widget>();
        options_row.set_layout<GUI::HorizontalBoxLayout>(0, 12);
        search_case = options_row.add<GUI::CheckBox>("Match case"_string);
        search_wrap = options_row.add<GUI::CheckBox>("Wrap around"_string);
        search_wrap->set_checked(true);

        auto& find_next_row = panel->add<GUI::Widget>();
        find_next_row.set_layout<GUI::HorizontalBoxLayout>(0, 6);
        find_next_row.add_spacer();
        auto& close_button = find_next_row.add<GUI::Button>("Close"_string);
        auto& replace_all_button = find_next_row.add<GUI::Button>("Replace All"_string);
        auto& replace_button = find_next_row.add<GUI::Button>("Replace"_string);
        replace_button.set_default(true);

        auto find_next = [&](GUI::TextEditor::SearchDirection direction, bool show_message) {
            auto needle = find_box->text();
            if (needle.is_empty())
                return false;
            auto wrap = search_wrap->is_checked() ? GUI::TextDocument::SearchShouldWrap::Yes : GUI::TextDocument::SearchShouldWrap::No;
            auto result = editor.find_text(needle, direction, wrap, false, search_case->is_checked());
            if (!result.is_valid() && show_message)
                GUI::MessageBox::show(window, ByteString::formatted("Not found: {}", needle), "Not found"sv, GUI::MessageBox::Type::Information);
            return result.is_valid();
        };

        auto matches = [&](StringView selected, StringView needle) {
            return search_case->is_checked() ? selected == needle : selected.equals_ignoring_ascii_case(needle);
        };

        auto replace_one = [&] {
            auto needle = find_box->text();
            if (needle.is_empty())
                return;
            if (editor.has_selection() && matches(editor.selected_text(), needle)) {
                editor.insert_at_cursor_or_replace_selection(replace_box->text());
                find_next(GUI::TextEditor::SearchDirection::Forward, false);
                return;
            }
            find_next(GUI::TextEditor::SearchDirection::Forward, true);
        };

        auto replace_all = [&] {
            auto needle = find_box->text();
            if (needle.is_empty())
                return;
            auto substitute = replace_box->text();
            auto length_delta = static_cast<ptrdiff_t>(Utf8View(substitute).length()) - static_cast<ptrdiff_t>(Utf8View(needle).length());
            auto match_case = search_case->is_checked();
            size_t count = 0;
            auto found = editor.document().find_next(needle, {}, GUI::TextDocument::SearchShouldWrap::No, false, match_case);
            if (!found.is_valid()) {
                GUI::MessageBox::show(window, ByteString::formatted("Not found: {}", needle), "Not found"sv, GUI::MessageBox::Type::Information);
                return;
            }
            while (found.is_valid()) {
                editor.set_selection(found);
                editor.insert_at_cursor_or_replace_selection(substitute);
                ++count;
                auto new_column = static_cast<ptrdiff_t>(found.end().column()) + length_delta;
                auto next_start = GUI::TextPosition(found.end().line(), new_column > 0 ? static_cast<size_t>(new_column) : 0);
                found = editor.document().find_next(needle, next_start, GUI::TextDocument::SearchShouldWrap::No, false, match_case);
            }
            GUI::MessageBox::show(window, ByteString::formatted("Replaced {} occurrence(s).", count), "Replace All"sv, GUI::MessageBox::Type::Information);
        };

        replace_button.on_click = [&](auto) { replace_one(); };
        replace_all_button.on_click = [&](auto) { replace_all(); };
        close_button.on_click = [&](auto) { replace_window->hide(); editor.set_focus(true); };

        find_box->on_return_pressed = [&] { replace_one(); };
        find_box->on_escape_pressed = [&] { replace_window->hide(); editor.set_focus(true); };
        replace_box->on_return_pressed = [&] { replace_one(); };
        replace_box->on_escape_pressed = [&] { replace_window->hide(); editor.set_focus(true); };

        // Pre-fill with the current selection.
        if (editor.has_selection()) {
            find_box->set_text(editor.selected_text());
            find_box->select_all();
        }
        replace_window->resize(380, 170);
        replace_window->show();
        find_box->set_focus(true);
    };
    edit_menu->add_action(GUI::Action::create("&Replace..."sv, { Mod_Ctrl, Key_H }, [&](auto&) { show_replace(); }));
    edit_menu->add_separator();
    edit_menu->add_action(editor.go_to_line_or_column_action());
    edit_menu->add_action(GUI::Action::create("&Time/Date"sv, { Mod_None, Key_F5 }, [&](auto&) {
        auto now = Core::DateTime::now().to_string();
        if (now.is_error())
            return;
        editor.insert_at_cursor_or_replace_selection(now.value());
    }));
    edit_menu->add_separator();
    edit_menu->add_action(editor.select_all_action());

    // --- Menu: Format ---------------------------------------------------

    auto format_menu = window->add_menu("&Format"_string);
    auto word_wrap_action = GUI::Action::create_checkable("Word &Wrap"sv, [&](auto& action) {
        auto wrap = action.is_checked();
        editor.set_wrapping_mode(wrap ? GUI::TextEditor::WrappingMode::WrapAnywhere : GUI::TextEditor::WrappingMode::NoWrap);
        Config::write_bool("Notepad"sv, "Format"sv, "WordWrap"sv, wrap);
    });
    auto word_wrap = Config::read_bool("Notepad"sv, "Format"sv, "WordWrap"sv, true);
    word_wrap_action->set_checked(word_wrap);
    editor.set_wrapping_mode(word_wrap ? GUI::TextEditor::WrappingMode::WrapAnywhere : GUI::TextEditor::WrappingMode::NoWrap);
    format_menu->add_action(*word_wrap_action);

    auto syntax_submenu = format_menu->add_submenu("S&yntax"_string);
    GUI::ActionGroup syntax_actions;
    syntax_actions.set_exclusive(true);
    auto add_syntax_action = [&](StringView text, SyntaxChoice choice) {
        auto action = GUI::Action::create_checkable(text, [&, choice](auto& action) {
            syntax_choice = choice;
            action.set_checked(true);
            apply_syntax();
        });
        if (choice == syntax_choice)
            action->set_checked(true);
        syntax_actions.add_action(*action);
        syntax_submenu->add_action(*action);
    };
    add_syntax_action("&Auto"sv, SyntaxChoice::Auto);
    add_syntax_action("Plain &Text"sv, SyntaxChoice::PlainText);
    syntax_submenu->add_separator();
    add_syntax_action("&C++"sv, SyntaxChoice::Cpp);
    add_syntax_action("CM&ake"sv, SyntaxChoice::CMake);
    add_syntax_action("&JavaScript"sv, SyntaxChoice::JavaScript);
    add_syntax_action("&Markdown"sv, SyntaxChoice::Markdown);
    add_syntax_action("&INI"sv, SyntaxChoice::INI);
    add_syntax_action("S&hell"sv, SyntaxChoice::Shell);
    add_syntax_action("&Git Commit"sv, SyntaxChoice::GitCommit);
    add_syntax_action("&GML"sv, SyntaxChoice::GML);
    add_syntax_action("&HTML"sv, SyntaxChoice::HTML);
    add_syntax_action("&CSS"sv, SyntaxChoice::CSS);
    add_syntax_action("&SQL"sv, SyntaxChoice::SQL);

    auto line_ending_submenu = format_menu->add_submenu("&Line Endings"_string);
    GUI::ActionGroup line_ending_actions;
    line_ending_actions.set_exclusive(true);
    auto add_line_ending_action = [&](StringView text, LineEnding ending) {
        auto action = GUI::Action::create_checkable(text, [&, ending](auto&) {
            current_format.line_ending = ending;
            sync_format_menus();
            update_statusbar();
        });
        if (ending == current_format.line_ending)
            action->set_checked(true);
        line_ending_actions.add_action(*action);
        line_ending_submenu->add_action(*action);
        line_ending_menu_actions.append({ ending, *action });
    };
    add_line_ending_action("&Windows (CRLF)"sv, LineEnding::Crlf);
    add_line_ending_action("&Unix (LF)"sv, LineEnding::Lf);
    add_line_ending_action("Old &Mac (CR)"sv, LineEnding::Cr);
    format_menu->add_separator();
    format_menu->add_action(GUI::Action::create("&Font..."sv, [&](auto&) {
        auto picker = GUI::FontPicker::construct(window, &editor.font(), false);
        if (picker->exec() == GUI::Dialog::ExecResult::OK) {
            auto font = picker->font();
            if (font) {
                editor.set_font(font.ptr());
                Config::write_string("Notepad"sv, "Text"sv, "Font"sv, font->qualified_name());
            }
        }
    }));

    // --- Menu: Help ----------------------------------------------------

    auto help_menu = window->add_menu("&Help"_string);
    help_menu->add_action(GUI::Action::create("&About Notepad"sv, [&](auto&) {
        GUI::MessageBox::show(window,
            "Notepad\n\nA native SerenityOS text editor.\nOpens UTF-8 / UTF-16 / GB18030 files,\nauto-detecting encoding and line endings,\nwith syntax highlighting and word wrap.\nPart of the Linux GUI port."sv,
            "About Notepad"sv, GUI::MessageBox::Type::Information);
    }));

    // --- Behaviour wiring ------------------------------------------------

    editor.on_modified_change = [&](bool modified) {
        window->set_modified(modified);
        update_title();
    };
    editor.on_change = [&] { update_statusbar(); };
    editor.on_selection_change = [&] { update_statusbar(); };
    editor.on_highlighter_change = [&] { update_statusbar(); };

    window->on_close_request = [&]() -> GUI::Window::CloseRequestDecision {
        if (confirm_discard())
            return GUI::Window::CloseRequestDecision::Close;
        return GUI::Window::CloseRequestDecision::StayOpen;
    };

    // Persisted font preference.
    if (auto font_name = Config::read_string("Notepad"sv, "Text"sv, "Font"sv, ""sv); !font_name.is_empty()) {
        if (auto font = Gfx::FontDatabase::the().get_by_name(font_name))
            editor.set_font(font);
    }

    update_title();
    update_statusbar();

    // Open a file passed on the command line (e.g. double-clicked in the file manager).
    if (arguments.strings.size() > 1) {
        if (arguments.strings.size() > 2) {
            GUI::MessageBox::show(window,
                "TextEditor can only open one file at a time!\n"sv,
                "Notepad"sv, GUI::MessageBox::Type::Information);
        }
        auto file = Core::File::open(ByteString { arguments.strings[1] }, Core::File::OpenMode::Read);
        if (file.is_error()) {
            GUI::MessageBox::show(window, ByteString::formatted("Unable to open {}.\n", arguments.strings[1]), "Error"sv, GUI::MessageBox::Type::Error);
        } else {
            load_from_stream(*file.value(), ByteString { arguments.strings[1] }, {});
        }
    }

    window->show();
    editor.set_focus(true);

    return app->exec();
}
