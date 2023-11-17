/*
    Copyright 2019-2023 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include <wx/rawbmp.h>

#include "noo_canvas.h"
#include "noo_app.h"
#include "noo_frame.h"
#include "../settings.h"

#if defined(_WIN32) && defined(USE_GL_CANVAS)
#include <GL/gl.h>
#include <GL/glext.h>
#endif

wxBEGIN_EVENT_TABLE(NooCanvas, CANVAS_CLASS)
EVT_PAINT(NooCanvas::draw)
EVT_SIZE(NooCanvas::resize)
EVT_KEY_DOWN(NooCanvas::pressKey)
EVT_KEY_UP(NooCanvas::releaseKey)
EVT_LEFT_DOWN(NooCanvas::pressScreen)
EVT_MOTION(NooCanvas::pressScreen)
EVT_LEFT_UP(NooCanvas::releaseScreen)
wxEND_EVENT_TABLE()

NooCanvas::NooCanvas(NooFrame *frame): CANVAS_CLASS(frame, wxID_ANY, CANVAS_PARAM), frame(frame)
{
#ifdef USE_GL_CANVAS
    // Prepare the OpenGL context
    context = new wxGLContext(this);
    SetCurrent(*context);

    // Prepare a texture for the framebuffer
    GLuint texture;
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif

    // Set focus so that key presses will be registered
    SetFocus();
    frame->SendSizeEvent();
}

void NooCanvas::drawScreen(int x, int y, int w, int h, int wb, int hb, uint32_t *buf)
{
#ifdef USE_GL_CANVAS
    // Set texture coordinates based on rotation
    static const uint8_t texCoords[] = { 0x4B, 0x2D, 0xD2 };
    uint8_t coords = texCoords[ScreenLayout::screenRotation];

    // Draw a screen with the given information
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wb, hb, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glBegin(GL_QUADS);
    glTexCoord2i((coords >> 0) & 1, (coords >> 1) & 1);
    glVertex2i(x + w, y + h);
    glTexCoord2i((coords >> 2) & 1, (coords >> 3) & 1);
    glVertex2i(x, y + h);
    glTexCoord2i((coords >> 4) & 1, (coords >> 5) & 1);
    glVertex2i(x, y);
    glTexCoord2i((coords >> 6) & 1, (coords >> 7) & 1);
    glVertex2i(x + w, y);
    glEnd();
#else
    // Create a bitmap for the screen
    wxBitmap bmp(wb, hb, 24);
    wxNativePixelData data(bmp);
    wxNativePixelData::Iterator iter(data);

    // Copy buffer data to the bitmap
    for (int i = 0; i < hb; i++)
    {
        wxNativePixelData::Iterator pixel = iter;
        for (int j = 0; j < wb; j++, pixel++)
        {
            uint32_t color = buf[i * wb + j];
            pixel.Red() = ((color >> 0) & 0xFF);
            pixel.Green() = ((color >> 8) & 0xFF);
            pixel.Blue() = ((color >> 16) & 0xFF);
        }
        iter.OffsetY(data, 1);
    }

    // Draw the bitmap, rotated and scaled
    wxPaintDC dc(this);
    wxImage img = bmp.ConvertToImage();
    if (ScreenLayout::screenRotation > 0)
        img = img.Rotate90(ScreenLayout::screenRotation == 1);
    img.Rescale(w, h, NooApp::screenFilter ? wxIMAGE_QUALITY_BILINEAR : wxIMAGE_QUALITY_NEAREST);
    bmp = wxBitmap(img);
    dc.DrawBitmap(bmp, wxPoint(x, y));
#endif
}

void NooCanvas::draw(wxPaintEvent &event)
{
    // Stop rendering if the program is closing
    if (finished)
        return;

#ifdef USE_GL_CANVAS
    // Clear the frame
    SetCurrent(*context);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#endif

    if (frame->getCore())
    {
        // Update the layout if GBA mode changed
        bool gba = (frame->getCore()->gbaMode && ScreenLayout::gbaCrop);
        if (gbaMode != gba)
        {
            gbaMode = gba;
            frame->SendSizeEvent();
        }

        // Emulation is limited by audio, so frames aren't always generated at a consistent rate
        // This can mess up frame pacing at higher refresh rates when frames are ready too soon
        // To solve this, use a software-based swap interval to wait before getting the next frame
        if (++frameCount >= swapInterval && frame->getCore()->gpu.getFrame(framebuffer, gba))
            frameCount = 0;

        // Shift the screen resolutions if high-res is enabled
        bool resShift = Settings::highRes3D;

        if (gbaMode)
        {
            // Draw the GBA screen
            drawScreen(layout.topX, layout.topY, layout.topWidth, layout.topHeight,
               240 << resShift, 160 << resShift, &framebuffer[0]);
        }
        else
        {
            // Draw the DS top and bottom screens
            drawScreen(layout.topX, layout.topY, layout.topWidth, layout.topHeight,
               256 << resShift, 192 << resShift, &framebuffer[0]);
            drawScreen(layout.botX, layout.botY, layout.botWidth, layout.botHeight,
               256 << resShift, 192 << resShift, &framebuffer[(256 * 192) << (resShift * 2)]);
        }
    }

    // Track the refresh rate and update the swap interval every second
    refreshRate++;
    std::chrono::duration<double> rateTime = std::chrono::steady_clock::now() - lastRateTime;
    if (rateTime.count() >= 1.0f)
    {
        swapInterval = (refreshRate + 5) / 60; // Margin of 5
        refreshRate = 0;
        lastRateTime = std::chrono::steady_clock::now();
    }

#ifdef USE_GL_CANVAS
    // Display the finished frame
    glFinish();
    SwapBuffers();
#endif
}

void NooCanvas::resize(wxSizeEvent &event)
{
    // Update the screen layout
    wxSize size = GetSize();
    layout.update(size.x, size.y, gbaMode);

    // Full screen breaks the minimum frame size, but changing to a different value fixes it
    // As a workaround, clear the minimum size on full screen and reset it shortly after
    frame->SetMinClientSize(sizeReset ? wxSize(0, 0) : wxSize(layout.minWidth, layout.minHeight));
    sizeReset -= (bool)sizeReset;

#ifdef USE_GL_CANVAS
    // Update the display dimensions
    SetCurrent(*context);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, size.x, size.y, 0, -1, 1);
    glViewport(0, 0, size.x, size.y);

    // Set filtering
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, NooApp::screenFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, NooApp::screenFilter ? GL_LINEAR : GL_NEAREST);
#endif
}

void NooCanvas::pressKey(wxKeyEvent &event)
{
    // Trigger a key press if a mapped key was pressed
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (event.GetKeyCode() == NooApp::keyBinds[i])
            frame->pressKey(i);
    }
}

void NooCanvas::releaseKey(wxKeyEvent &event)
{
    // Trigger a key release if a mapped key was released
    for (int i = 0; i < MAX_KEYS; i++)
    {
        if (event.GetKeyCode() == NooApp::keyBinds[i])
            frame->releaseKey(i);
    }
}

void NooCanvas::pressScreen(wxMouseEvent &event)
{
    // Ensure the left mouse button is clicked
    if (!frame->isRunning() || !event.LeftIsDown()) return;

    // Determine the touch position relative to the emulated touch screen
    int touchX = layout.getTouchX(event.GetX(), event.GetY());
    int touchY = layout.getTouchY(event.GetX(), event.GetY());

    // Send the touch coordinates to the core
    frame->getCore()->input.pressScreen();
    frame->getCore()->spi.setTouch(touchX, touchY);
}

void NooCanvas::releaseScreen(wxMouseEvent &event)
{
    // Send a touch release to the core
    if (frame->isRunning())
    {
        frame->getCore()->input.releaseScreen();
        frame->getCore()->spi.clearTouch();
    }
}
