/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SpectrumWidget.h"
#include <AK/IntegralMath.h>
#include <AK/Math.h>
#include <LibDSP/FFT.h>
#include <LibDSP/Window.h>
#include "TTSkin.h"
#include <LibGUI/Painter.h>
#include <math.h>

namespace {

// Cell heights for the segmented look (each "LED cell" is 5px + 1px gap).
constexpr int CellHeight = 5;
constexpr int CellGap = 1;
constexpr int CellStride = CellHeight + CellGap;

Gfx::Color cell_color(int cell_index, int cell_count)
{
    // Green at the bottom, yellow in the middle, red near the top.
    float position = cell_count <= 1 ? 0.0f : static_cast<float>(cell_index) / static_cast<float>(cell_count - 1);
    if (position < 0.55f)
        return Gfx::Color::from_rgb(0x2ee06a);
    if (position < 0.8f)
        return Gfx::Color::from_rgb(0xffe14d);
    return Gfx::Color::from_rgb(0xff5340);
}

}

SpectrumWidget::SpectrumWidget()
    : m_render_buffer(MUST(FixedArray<float>::create(fft_size / 2)))
{
    m_fft_window = DSP::Window<float>::hann<fft_size>();
    for (auto& bar : m_displayed)
        bar = 0;
    for (auto& peak : m_peak_falls)
        peak = 0;
    start_timer(REFRESH_TIME_MILLISECONDS);
}

void SpectrumWidget::compute_bars(float* groups, size_t group_count)
{
    VERIFY(group_count == bar_count);

    for (size_t i = 0; i < fft_size / 2; i++)
        m_fft_samples[i] = m_previous_samples[i] * m_fft_window[i];
    for (size_t i = 0; i < fft_size / 2; i++)
        m_fft_samples[i + fft_size / 2] = m_render_buffer[i] * m_fft_window[i + fft_size / 2];

    AK::TypedTransfer<float>::copy(m_previous_samples.data(), m_render_buffer.data(), m_render_buffer.size());

    DSP::fft(m_fft_samples.span(), false);

    // Logarithmically-spaced bars from ~20 Hz to the cutoff bin.
    size_t constexpr cutoff = fft_size - 32;
    float const log_bar_size = static_cast<float>(bar_count) / AK::log2(static_cast<float>(fft_size));

    Array<float, bar_count> magnitudes {};
    for (size_t i = 0; i < bar_count; ++i) {
        auto const bar_start = i == 0 ? 0 : static_cast<size_t>(floor(AK::pow(2.f, static_cast<float>(i) / log_bar_size)));
        auto const bar_end = clamp(static_cast<size_t>(floor(AK::pow(2.f, static_cast<float>(i + 1) / log_bar_size))), bar_start + 1, cutoff);
        float sum = 0;
        for (size_t j = bar_start; j < bar_end; ++j)
            sum += m_fft_samples[j].magnitude();
        magnitudes[i] = sum / static_cast<float>(bar_end - bar_start);
    }

    float const max_peak_value = AK::sqrt(static_cast<float>(fft_size * 2));
    for (size_t i = 0; i < bar_count; i++) {
        magnitudes[i] = AK::log(magnitudes[i] + 1) / AK::log(max_peak_value);
        // Boost high frequencies so the right half of the display moves too.
        magnitudes[i] *= 1 + 2.0f * (static_cast<float>(i) - bar_count / 3.0f) / static_cast<float>(bar_count);
        magnitudes[i] = clamp(magnitudes[i], 0.0f, 1.0f);
    }

    for (size_t i = 0; i < bar_count; i++)
        groups[i] = magnitudes[i];
}

void SpectrumWidget::paint_event(GUI::PaintEvent& event)
{
    GUI::Frame::paint_event(event);
    GUI::Painter painter(*this);

    painter.add_clip_rect(event.rect());

    auto const& c = TTSkin::colors();
    auto inner = frame_inner_rect();
    // Inset panel with a subtle sheen at the top; follows the active skin.
    painter.fill_rect_with_gradient(Gfx::Orientation::Vertical, inner,
        c.inset_background.lightened(1.35f), c.inset_background.darkened(0.6f));

    // Fill the render buffer with the current slice of audio.
    if (m_sample_buffer.size() > 0) {
        size_t buffer_position = (m_frame_count * REFRESH_TIME_MILLISECONDS) * static_cast<size_t>(m_samplerate > 0 ? m_samplerate : 44100) / 1000;
        if (buffer_position + m_render_buffer.size() >= m_sample_buffer.size())
            buffer_position = m_sample_buffer.size() - m_render_buffer.size();
        AK::TypedTransfer<float>::copy(m_render_buffer.data(), m_sample_buffer.span().slice(buffer_position).data(), m_render_buffer.size());
    }

    Array<float, bar_count> bars {};
    bool const has_audio = m_sample_buffer.size() > 0;
    if (has_audio)
        compute_bars(bars.data(), bar_count);

    int const horizontal_margin = 8;
    int const top_margin = 6;
    int const bottom_margin = 4;
    int const cell_gap = 2;
    int const usable_width = inner.width() - horizontal_margin * 2;
    int const bar_width = max(2, (usable_width - cell_gap * (static_cast<int>(bar_count) - 1)) / static_cast<int>(bar_count));
    int const height = inner.height() - top_margin - bottom_margin;
    int const cell_count = max(1, height / CellStride);

    for (size_t i = 0; i < bar_count; i++) {
        float target = has_audio ? bars[i] : 0.0f;
        // Smooth the falloff so bars look like real VU meters.
        if (target >= m_displayed[i])
            m_displayed[i] = target;
        else
            m_displayed[i] = max(target, m_displayed[i] - 0.04f);

        int x = inner.x() + horizontal_margin + static_cast<int>(i) * (bar_width + cell_gap);
        int lit_cells = static_cast<int>(roundf(m_displayed[i] * static_cast<float>(cell_count)));
        lit_cells = clamp(lit_cells, 0, cell_count);

        // Falling peak dot.
        int peak_cell = lit_cells;
        if (m_peak_falls[i] < peak_cell)
            m_peak_falls[i] = peak_cell;
        else
            m_peak_falls[i] = max(m_peak_falls[i] - 1, 0);

        for (int cell = 0; cell < cell_count; cell++) {
            int y = inner.bottom() - bottom_margin - (cell + 1) * CellStride + CellGap;
            bool lit = cell < lit_cells;
            Gfx::Color color = lit ? cell_color(cell, cell_count) : c.inset_background.lightened(1.7f);
            painter.fill_rect(Gfx::IntRect(x, y, bar_width, CellHeight), color);
        }

        if (m_peak_falls[i] > 0) {
            int cell = clamp(m_peak_falls[i] - 1, 0, cell_count - 1);
            int y = inner.bottom() - bottom_margin - (cell + 1) * CellStride + CellGap;
            painter.fill_rect(Gfx::IntRect(x, y, bar_width, CellHeight), c.text);
        }
    }
}
