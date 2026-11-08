/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Mohsan Ali <mohsan0073@gmail.com>
 * Copyright (c) 2022, Mustafa Quraish <mustafa@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Caoimhe Byrne <caoimhebyrne06@gmail.com>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ViewWidget.h"
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/MappedFile.h>
#include <LibCore/MimeData.h>
#include <LibCore/Timer.h>
#include <LibFileSystemAccessClient/Client.h>
#include <LibGUI/Application.h>
#include <LibGUI/MessageBox.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/Orientation.h>
#include <LibGfx/Palette.h>
#include <LibImageDecoderClient/Client.h>

namespace ImageViewer {

void VectorImage::flip(Gfx::Orientation orientation)
{
    if (orientation == Gfx::Orientation::Horizontal)
        apply_transform(Gfx::AffineTransform {}.scale(-1, 1));
    else
        apply_transform(Gfx::AffineTransform {}.scale(1, -1));
}

void VectorImage::rotate(Gfx::RotationDirection rotation_direction)
{
    if (rotation_direction == Gfx::RotationDirection::Clockwise)
        apply_transform(Gfx::AffineTransform {}.rotate_radians(AK::Pi<float> / 2));
    else
        apply_transform(Gfx::AffineTransform {}.rotate_radians(-AK::Pi<float> / 2));
    m_size = { m_size.height(), m_size.width() };
}

void VectorImage::draw_into(Gfx::Painter& painter, Gfx::IntRect const& dest, Gfx::ScalingMode) const
{
    m_vector->draw_into(painter, dest, m_transform);
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VectorImage::bitmap(Optional<Gfx::IntSize> ideal_size) const
{
    return m_vector->bitmap(ideal_size.value_or(size()), m_transform);
}

void BitmapImage::flip(Gfx::Orientation orientation)
{
    m_bitmap = m_bitmap->flipped(orientation).release_value_but_fixme_should_propagate_errors();
}

void BitmapImage::rotate(Gfx::RotationDirection rotation)
{
    m_bitmap = m_bitmap->rotated(rotation).release_value_but_fixme_should_propagate_errors();
}

void BitmapImage::draw_into(Gfx::Painter& painter, Gfx::IntRect const& dest, Gfx::ScalingMode scaling_mode) const
{
    painter.draw_scaled_bitmap(dest, *m_bitmap, m_bitmap->rect(), 1.0f, scaling_mode);
}

ViewWidget::ViewWidget()
    : m_timer(Core::Timer::try_create().release_value_but_fixme_should_propagate_errors())
{
    set_fill_with_background_color(false);
}

void ViewWidget::clear()
{
    m_timer->stop();
    m_animation.clear();
    reset_crop_state();
    m_image = nullptr;
    if (on_image_change)
        on_image_change(m_image);
    set_original_rect({});
    m_path = {};

    reset_view();
    update();
}

void ViewWidget::flip(Gfx::Orientation orientation)
{
    m_image->flip(orientation);
    scale_image_for_window();
}

void ViewWidget::rotate(Gfx::RotationDirection rotation_direction)
{
    m_image->rotate(rotation_direction);
    scale_image_for_window();
}

bool ViewWidget::is_next_available() const
{
    if (m_current_index.has_value())
        return m_current_index.value() + 1 < m_files_in_same_dir.size();
    return false;
}

bool ViewWidget::is_previous_available() const
{
    if (m_current_index.has_value())
        return m_current_index.value() > 0;
    return false;
}

// FIXME: Convert to `String` & use LibFileSystemAccessClient + `Core::System::unveil(nullptr, nullptr)`
//        - Converting to String is not super-trivial due to the LexicalPath usage, while we can do a bunch of
//          String::from_byte_string() and String.to_byte_string(), it is quite ugly to read and
//          probably not the best approach.
//
//        - If we go full-unveil (`Core::System::unveil(nullptr, nullptr)`) this functionality does not work,
//          we can not access the list of contents of a directory through LibFileSystemAccessClient at the moment.
Vector<ByteString> ViewWidget::load_files_from_directory(ByteString const& path) const
{
    Vector<ByteString> files_in_directory;

    auto current_dir = LexicalPath(path).parent().string();
    // FIXME: Propagate errors
    (void)Core::Directory::for_each_entry(current_dir, Core::DirIterator::Flags::SkipDots, [&](auto const& entry, auto const& directory) -> ErrorOr<IterationDecision> {
        auto full_path = LexicalPath::join(directory.path().string(), entry.name).string();
        if (Gfx::Bitmap::is_path_a_supported_image_format(full_path))
            files_in_directory.append(full_path);
        return IterationDecision::Continue;
    });
    return files_in_directory;
}

void ViewWidget::set_path(String const& path)
{
    m_path = path;
    m_files_in_same_dir = load_files_from_directory(path.to_byte_string());
    m_current_index = m_files_in_same_dir.find_first_index(path.to_byte_string());
}

void ViewWidget::navigate(Directions direction)
{
    if (!m_current_index.has_value()) {
        return;
    }

    auto index = m_current_index.value();
    if (direction == Directions::Back) {
        index--;
    } else if (direction == Directions::Forward) {
        index++;
    } else if (direction == Directions::First) {
        index = 0;
    } else if (direction == Directions::Last) {
        index = m_files_in_same_dir.size() - 1;
    }

    auto result = FileSystemAccessClient::Client::the().request_file_read_only_approved(window(), m_files_in_same_dir.at(index));
    if (result.is_error())
        return;

    m_current_index = index;

    auto value = result.release_value();
    open_file(MUST(String::from_byte_string(value.filename())), value.stream());
}

void ViewWidget::doubleclick_event(GUI::MouseEvent&)
{
    on_doubleclick();
}

void ViewWidget::paint_event(GUI::PaintEvent& event)
{
    Frame::paint_event(event);

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());
    painter.add_clip_rect(frame_inner_rect());

    Gfx::StylePainter::paint_transparency_grid(painter, frame_inner_rect(), palette());

    if (m_image)
        m_image->draw_into(painter, content_rect(), m_scaling_mode);

    if (m_crop_mode && m_image)
        paint_crop_overlay(painter);
}

void ViewWidget::paint_crop_overlay(Gfx::Painter& painter)
{
    auto const selection_image_rect = current_crop_rect();

    // Dim the area outside of the selection (if any) to make it obvious
    // what will be kept.
    if (selection_image_rect.has_value()) {
        auto const selection_frame = enclosing_int_rect(content_to_frame_rect(*selection_image_rect)).intersected(frame_inner_rect());
        auto const dim_color = Gfx::Color { Gfx::Color::Black }.with_alpha(128);
        auto const inner = frame_inner_rect();

        painter.fill_rect({ inner.x(), inner.y(), inner.width(), selection_frame.y() - inner.y() }, dim_color);
        painter.fill_rect({ inner.x(), selection_frame.bottom() + 1, inner.width(), inner.bottom() - selection_frame.bottom() }, dim_color);
        painter.fill_rect({ inner.x(), selection_frame.y(), selection_frame.x() - inner.x(), selection_frame.height() }, dim_color);
        painter.fill_rect({ selection_frame.right() + 1, selection_frame.y(), inner.right() - selection_frame.right(), selection_frame.height() }, dim_color);

        // Dashed border in alternating black/white dashes, visible on any content.
        auto const top_left = selection_frame.top_left();
        auto const bottom_right = selection_frame.bottom_right();
        auto const top_right = Gfx::IntPoint { bottom_right.x(), top_left.y() };
        auto const bottom_left = Gfx::IntPoint { top_left.x(), bottom_right.y() };
        painter.draw_line(top_left, top_right, Gfx::Color::White, 1, Gfx::LineStyle::Dashed, Gfx::Color::Black);
        painter.draw_line(top_left, bottom_left, Gfx::Color::White, 1, Gfx::LineStyle::Dashed, Gfx::Color::Black);
        painter.draw_line(bottom_left, bottom_right, Gfx::Color::White, 1, Gfx::LineStyle::Dashed, Gfx::Color::Black);
        painter.draw_line(top_right, bottom_right, Gfx::Color::White, 1, Gfx::LineStyle::Dashed, Gfx::Color::Black);

        // Size hint in the bottom-right corner of the selection.
        auto const size_text = selection_image_rect->size().to_byte_string();
        auto const text_width = static_cast<int>(painter.font().width(size_text)) + 1;
        auto const text_height = static_cast<int>(painter.font().pixel_metrics().line_spacing());
        auto text_rect = Gfx::IntRect { bottom_right.x() - text_width - 12, bottom_right.y() - text_height - 8, text_width + 8, text_height + 4 };
        text_rect = text_rect.intersected(frame_inner_rect());
        painter.fill_rect(text_rect, Gfx::Color { Gfx::Color::Black }.with_alpha(180));
        painter.draw_text(text_rect, size_text, Gfx::TextAlignment::Center, Gfx::Color::White);
    } else if (m_crop_drag_in_progress) {
        // Selection collapsed to nothing; still show a hint line? Draw nothing.
    }
}

void ViewWidget::mousedown_event(GUI::MouseEvent& event)
{
    if (m_crop_mode && event.button() == GUI::MouseButton::Primary) {
        auto content_position = content_position_clamped(event.position());
        if (content_position.has_value()) {
            m_crop_drag_in_progress = true;
            m_crop_drag_anchor = *content_position;
            m_crop_rect = Gfx::IntRect::from_two_points(*content_position, *content_position);
            if (on_crop_selection_change)
                on_crop_selection_change();
            update();
        }
        event.accept();
        return;
    }
    if (event.button() == GUI::MouseButton::Primary)
        start_panning(event.position());
    GUI::AbstractZoomPanWidget::mousedown_event(event);
}

void ViewWidget::mousemove_event(GUI::MouseEvent& event)
{
    if (m_crop_drag_in_progress) {
        auto content_position = content_position_clamped(event.position());
        if (content_position.has_value() && m_crop_drag_anchor.has_value()) {
            m_crop_rect = Gfx::IntRect::from_two_points(*m_crop_drag_anchor, *content_position);
            update();
        }
        event.accept();
        return;
    }
    GUI::AbstractZoomPanWidget::mousemove_event(event);
}

void ViewWidget::mouseup_event(GUI::MouseEvent& event)
{
    if (m_crop_drag_in_progress && event.button() == GUI::MouseButton::Primary) {
        m_crop_drag_in_progress = false;
        m_crop_drag_anchor.clear();
        auto const selection = current_crop_rect();
        if (!selection.has_value() || selection->width() < 2 || selection->height() < 2)
            m_crop_rect.clear();
        if (on_crop_selection_change)
            on_crop_selection_change();
        update();
        event.accept();
        return;
    }
    if (event.button() == GUI::MouseButton::Primary)
        stop_panning();
    GUI::AbstractZoomPanWidget::mouseup_event(event);
}

void ViewWidget::open_file(String const& path, Core::File& file)
{
    auto open_result = try_open_file(path, file);
    if (open_result.is_error()) {
        auto error = open_result.release_error();
        auto user_error_message = String::formatted("Failed to open the image: {}.", error).release_value_but_fixme_should_propagate_errors();

        GUI::MessageBox::show_error(nullptr, user_error_message);
    }
}

ErrorOr<void> ViewWidget::try_open_file(String const& path, Core::File& file)
{
    auto file_data = TRY(file.read_until_eof());
    bool is_animated = false;
    size_t loop_count = 0;
    Vector<Animation::Frame> frames;
    // Note: Doing this check only requires reading the header of images
    // (so if the image is not vector graphics it can be still be decoded OOP).
    if (auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(file_data)); decoder && decoder->natural_frame_format() == Gfx::NaturalFrameFormat::Vector) {
        // Use in-process decoding for vector graphics.
        is_animated = decoder->is_animated();
        loop_count = decoder->loop_count();
        frames.ensure_capacity(decoder->frame_count());
        for (u32 i = 0; i < decoder->frame_count(); i++) {
            auto frame_data = TRY(decoder->vector_frame(i));
            frames.unchecked_append({ VectorImage::create(*frame_data.image), frame_data.duration });
        }
    } else {
        // Out-of-process decoding through the ImageDecoder service.
        // The Linux GUI port does not run that service (there is no
        // /tmp/session/<sid>/portal/image socket), so the connect fails and
        // every raster image used to open with "connect: No such file or
        // directory". Fall back to in-process decoding, exactly like
        // FileSystemModel's thumbnailer does.
        Optional<ImageDecoderClient::DecodedImage> decoded;
        if (auto client = ImageDecoderClient::Client::try_create(); !client.is_error()) {
            auto mime_type = Core::guess_mime_type_based_on_filename(path);
            // FIXME: Refactor file opening to be more async-aware, and don't await this promise
            decoded = TRY(client.value()->decode_image(file_data, {}, {}, OptionalNone {}, mime_type)->await());
        } else {
            auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(file_data, Core::guess_mime_type_based_on_filename(path)));
            if (!decoder)
                return Error::from_string_literal("No image decoder available for this format");
            ImageDecoderClient::DecodedImage local;
            local.is_animated = decoder->is_animated();
            local.loop_count = static_cast<u32>(decoder->loop_count());
            auto frame_count = max<size_t>(1, decoder->frame_count());
            local.frames.ensure_capacity(frame_count);
            for (size_t i = 0; i < frame_count; ++i) {
                auto frame = TRY(decoder->frame(i));
                if (!frame.image)
                    return Error::from_string_literal("Image frame could not be decoded");
                local.frames.empend(frame.image.release_nonnull(), static_cast<u32>(frame.duration));
            }
            decoded = move(local);
        }

        is_animated = decoded->is_animated;
        loop_count = decoded->loop_count;
        frames.ensure_capacity(decoded->frames.size());
        for (auto& frame : decoded->frames)
            frames.unchecked_append({ BitmapImage::create(frame.bitmap, decoded->scale), int(frame.duration) });
    }

    clear();

    m_current_frame_index = 0;
    m_loops_completed = 0;
    m_image = frames[0].image;
    if (is_animated && frames.size() > 1) {
        m_animation = Animation { loop_count, move(frames) };
    }

    set_original_rect(m_image->rect());

    if (m_animation.has_value()) {
        auto const& first_frame = m_animation->frames[0];
        m_timer->set_interval(first_frame.duration);
        m_timer->on_timeout = [this] { animate(); };
        m_timer->start();
    } else {
        m_timer->stop();
    }

    set_path(path);
    GUI::Application::the()->set_most_recently_open_file(path.to_byte_string());

    if (on_image_change)
        on_image_change(m_image);

    if (scaled_for_first_image())
        scale_image_for_window();
    else
        reset_view();

    return {};
}

void ViewWidget::drag_enter_event(GUI::DragEvent& event)
{
    if (event.mime_data().has_urls())
        event.accept();
}

void ViewWidget::drop_event(GUI::DropEvent& event)
{
    event.accept();
    if (on_drop)
        on_drop(event);
}

void ViewWidget::resize_event(GUI::ResizeEvent& event)
{
    event.accept();
    scale_image_for_window();
}

void ViewWidget::scale_image_for_window()
{
    if (!m_image)
        return;

    set_original_rect(m_image->rect());
    fit_content_to_view(GUI::AbstractZoomPanWidget::FitType::Both);
}

void ViewWidget::resize_window()
{
    if (window()->is_fullscreen() || window()->is_maximized())
        return;

    auto absolute_bitmap_rect = content_rect();
    absolute_bitmap_rect.translate_by(window()->rect().top_left());

    if (!m_image)
        return;

    auto new_size = content_rect().size();

    if (new_size.width() < 300)
        new_size.set_width(300);
    if (new_size.height() < 200)
        new_size.set_height(200);

    if (new_size.width() > 500)
        new_size = { 500, 500 * absolute_bitmap_rect.height() / absolute_bitmap_rect.width() };
    if (new_size.height() > 500)
        new_size = { 500 * absolute_bitmap_rect.width() / absolute_bitmap_rect.height(), 500 };

    new_size.set_height(new_size.height() + m_toolbar_height);
    window()->resize(new_size);
    scale_image_for_window();
}

void ViewWidget::set_image(Image const* image)
{
    if (m_image == image)
        return;
    m_image = image;
    set_original_rect(m_image->rect());
    update();
}

// Same as ImageWidget::animate(), you probably want to keep any changes in sync
void ViewWidget::animate()
{
    if (!m_animation.has_value())
        return;

    m_current_frame_index = (m_current_frame_index + 1) % m_animation->frames.size();

    auto const& current_frame = m_animation->frames[m_current_frame_index];
    set_image(current_frame.image);

    if ((int)current_frame.duration != m_timer->interval()) {
        m_timer->restart(current_frame.duration);
    }

    if (m_current_frame_index == m_animation->frames.size() - 1) {
        ++m_loops_completed;
        if (m_loops_completed > 0 && m_loops_completed == m_animation->loop_count) {
            m_timer->stop();
        }
    }
}

void ViewWidget::set_scaling_mode(Gfx::ScalingMode scaling_mode)
{
    m_scaling_mode = scaling_mode;
    update();
}

void ViewWidget::reset_crop_state()
{
    m_crop_mode = false;
    m_crop_drag_in_progress = false;
    m_crop_drag_anchor.clear();
    m_crop_rect.clear();
    set_override_cursor(Gfx::StandardCursor::Arrow);
    if (on_crop_mode_change)
        on_crop_mode_change(false);
    if (on_crop_selection_change)
        on_crop_selection_change();
}

Optional<Gfx::IntRect> ViewWidget::current_crop_rect() const
{
    if (!m_image || !m_crop_rect.has_value())
        return {};
    auto const rect = m_crop_rect->intersected(m_image->rect());
    if (rect.is_empty())
        return {};
    return rect;
}

Optional<Gfx::IntPoint> ViewWidget::content_position_clamped(Gfx::IntPoint frame_position) const
{
    if (!m_image)
        return {};
    auto const position = frame_to_content_position(frame_position);
    auto const size = m_image->size();
    return Gfx::IntPoint {
        clamp(static_cast<int>(roundf(position.x())), 0, size.width()),
        clamp(static_cast<int>(roundf(position.y())), 0, size.height()),
    };
}

void ViewWidget::set_crop_mode(bool enabled)
{
    if (m_crop_mode == enabled)
        return;

    m_crop_mode = enabled;
    m_crop_drag_in_progress = false;
    m_crop_drag_anchor.clear();

    if (enabled) {
        stop_panning();
        set_override_cursor(Gfx::StandardCursor::Crosshair);
    } else {
        m_crop_rect.clear();
        set_override_cursor(Gfx::StandardCursor::Arrow);
    }

    if (on_crop_mode_change)
        on_crop_mode_change(m_crop_mode);
    if (on_crop_selection_change)
        on_crop_selection_change();
    update();
}

void ViewWidget::clear_crop_selection()
{
    m_crop_rect.clear();
    m_crop_drag_in_progress = false;
    m_crop_drag_anchor.clear();
    if (on_crop_selection_change)
        on_crop_selection_change();
    update();
}

RefPtr<Image> ViewWidget::crop_image_to_rect(Image const& image, Gfx::IntRect const& image_crop_rect)
{
    auto bitmap_or_error = image.bitmap({});
    if (bitmap_or_error.is_error())
        return nullptr;
    auto bitmap = bitmap_or_error.release_value();

    // The underlying bitmap may be at a different scale than the displayed
    // content-space rectangle (e.g. high-DPI images), so map the crop rect
    // into bitmap pixel coordinates.
    auto const scale_x = static_cast<float>(bitmap->width()) / static_cast<float>(image.size().width());
    auto const scale_y = static_cast<float>(bitmap->height()) / static_cast<float>(image.size().height());

    int const x0 = clamp(static_cast<int>(floorf(image_crop_rect.left() * scale_x)), 0, bitmap->width());
    int const y0 = clamp(static_cast<int>(floorf(image_crop_rect.top() * scale_y)), 0, bitmap->height());
    int const x1 = clamp(static_cast<int>(ceilf(image_crop_rect.right() * scale_x)), x0, bitmap->width());
    int const y1 = clamp(static_cast<int>(ceilf(image_crop_rect.bottom() * scale_y)), y0, bitmap->height());
    auto const src_rect = Gfx::IntRect { x0, y0, x1 - x0, y1 - y0 };
    if (src_rect.is_empty())
        return nullptr;
    auto cropped_or_error = bitmap->cropped(src_rect);
    if (cropped_or_error.is_error())
        return nullptr;
    return BitmapImage::create(cropped_or_error.release_value(), { 1, 1 });
}

bool ViewWidget::apply_crop()
{
    if (!m_image)
        return false;
    auto const selection = current_crop_rect();
    if (!selection.has_value())
        return false;

    // Crop every animation frame too so animated images stay consistent.
    Vector<RefPtr<Image>> cropped_images;
    if (m_animation.has_value()) {
        cropped_images.ensure_capacity(m_animation->frames.size());
        for (auto const& frame : m_animation->frames) {
            auto cropped = crop_image_to_rect(*frame.image, *selection);
            if (!cropped)
                return false;
            cropped_images.unchecked_append(move(cropped));
        }
    } else {
        auto cropped = crop_image_to_rect(*m_image, *selection);
        if (!cropped)
            return false;
        cropped_images.unchecked_append(move(cropped));
    }

    if (m_animation.has_value()) {
        for (size_t i = 0; i < m_animation->frames.size(); ++i)
            m_animation->frames[i].image = cropped_images[i];
        m_image = m_animation->frames[m_current_frame_index].image;
    } else {
        m_image = move(cropped_images[0]);
    }

    m_crop_rect.clear();
    m_crop_drag_in_progress = false;
    m_crop_drag_anchor.clear();

    // Applying the crop leaves crop mode; the caller syncs its toggle via on_crop_mode_change.
    m_crop_mode = false;
    set_override_cursor(Gfx::StandardCursor::Arrow);
    if (on_crop_mode_change)
        on_crop_mode_change(false);
    if (on_crop_selection_change)
        on_crop_selection_change();

    scale_image_for_window();
    if (on_image_change)
        on_image_change(m_image);
    update();
    return true;
}

}
