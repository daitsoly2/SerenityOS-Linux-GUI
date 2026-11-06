/*
 * Copyright (c) 2026, the SerenityOS Linux GUI port contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/String.h>  // AK/Complex.h's formatter uses AK::String
#include <AK/Format.h> // AK/Complex.h specializes AK::Formatter and expects this to be included first.
#include <AK/Complex.h>
#include <AK/FixedArray.h>
#include <AK/TypedTransfer.h>
#include <LibAudio/Sample.h>
#include <LibGUI/Frame.h>
#include <LibGUI/Painter.h>

// Classic 千千静听-style spectrum analyzer: segmented LED bars with falling
// peak dots on a near-black inset panel.
class SpectrumWidget final : public GUI::Frame {
    C_OBJECT(SpectrumWidget)

public:
    virtual ~SpectrumWidget() override = default;

    void set_buffer(FixedArray<Audio::Sample> const& buffer)
    {
        if (buffer.is_empty())
            return;

        if (m_sample_buffer.size() != buffer.size())
            m_sample_buffer.resize(buffer.size());

        for (size_t i = 0; i < buffer.size(); i++)
            m_sample_buffer[i] = (buffer[i].left + buffer[i].right) / 2.f;

        m_frame_count = 0;
    }

    void set_samplerate(int samplerate) { m_samplerate = samplerate; }

    void reset_buffer()
    {
        m_sample_buffer.clear();
        m_render_buffer.fill_with(.0f);
        for (auto& peak : m_peak_falls)
            peak = 0;
        for (auto& bar : m_displayed)
            bar = 0;
        m_frame_count = 0;
        update();
    }

    virtual void paint_event(GUI::PaintEvent& event) override;
    virtual void timer_event(Core::TimerEvent&) override
    {
        update();
        m_frame_count++;
    }

private:
    SpectrumWidget();

    void compute_bars(float* groups, size_t group_count);

    static constexpr size_t fft_size = 512;
    static constexpr size_t bar_count = 32;
    static constexpr size_t REFRESH_TIME_MILLISECONDS = 30;

    int m_samplerate { -1 };
    size_t m_frame_count { 0 };
    Vector<float> m_sample_buffer;
    FixedArray<float> m_render_buffer;

    Array<Complex<float>, fft_size> m_fft_samples {};
    Array<float, fft_size> m_fft_window {};
    Array<float, fft_size / 2> m_previous_samples {};
    Array<float, bar_count> m_displayed {};
    Array<int, bar_count> m_peak_falls {};
};
