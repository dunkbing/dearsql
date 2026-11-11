#pragma once

#include <string>

// docs screenshots and run videos, kept out of the app and of imshell: after each frame the
// test loop calls afterFrame(), which draws the frame's draw data again offscreen
namespace frame_capture {
    // the next frame at `scale`, as a png without the title bar strip
    void requestPng(const std::string& path, float scale);
    bool pending();
    // every frame from here on at `scale`, piped raw to ffmpeg as an mp4
    bool startRecording(const char* path, float scale, int width, int height);
    void stopRecording();
    void afterFrame(); // after shell().renderFrame(), from the test loop
} // namespace frame_capture
