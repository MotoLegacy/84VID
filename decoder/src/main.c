/*
84VID/VID84 Video Decoder and Player - v1.0

The Encoder, Decoder, and Spec are in the Public Domain.
No warranty implied; use at your own risk.

Features:
- 240x240 Resolution Canvas with Integer Down-Scaling.
- Supporting Custom FPS/Refresh Rates up to 63Hz (soft limit).
- Black-On-White (No color, no grayscale).

Video File Specification:

typedef struct {
    unsigned char[8] magic;     // 'magic', really an identifier. always '84VID'.
    unsigned char refresh_rate; // FPS/Refresh Rate, 1 byte.
    unsigned char version;      // Format version, 1 byte.
    unsigned char scale_factor; // The Integer Down-scale factor (ex. 2 for 120x120).
} vid84header_t;

84VID/VID84 (interchangeable) does not contain any features to
optimize frames by detecting duplicate data. As such, it was
important to keep data storage tiny, and simple.

Frames always begin with 0xFF. The bytes in between frames will always be divisible
by four, this is because the "image" data is actually a sequence of rectangles.

typedef struct {
    unsigned char x;            // First X Coordinate.
    unsigned char y;            // First Y Coordinate.
    unsigned char x2;           // Second X Coordinate.
    unsigned char y2;           // Second Y Coordinate.
} vid84rect_t;

This is far more efficient than storing the image data as a 2D array of pixel coordinates.
As an example..

240 x 240 x 60 = 3,456,000bytes / 3,375kB / 3.3mB per second @ 60Hz..
..this format would suck!

That being said, the codec's actual size per frame is dependent on the complexity of the
"mesh" of rectangle vertices created. It is entirely possible someone could have a dithered
pattern of on and off pixels resulting in a larger size per frame than the above method.

There are no start and end indicators for the rectangles. The decoder and player has
to keep track interally of the values.

The video will always end with 0xFE.
*/

#include <ti/getcsc.h>
#include <sys/timers.h>
#include <graphx.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

// Video file header. Only one can be included at a time.
#include "sample.h"

unsigned char video_fps;
unsigned char video_version;
unsigned char video_scale_factor;

bool retrieve_data_from_video(void)
{
    // Verify the magic, err, more like just the identifier but whatever. I was tired :s
    if (video_bin[0] != '8' || video_bin[1] != '4' || video_bin[2] != 'V' ||
    video_bin[3] != 'I' || video_bin[4] != 'D')
        return false;

    // Verify the framerate isn't above stock (63.5Hz) and is not zero.
    video_fps = video_bin[5];
    if ((int)video_fps > 63 || (int)video_fps == 0)
        return false;

    // Version check
    video_version = video_bin[6];
    if ((int)video_version != 1)
        return false;

    // Verify the scale factor
    video_scale_factor = video_bin[7];
    if ((int)video_scale_factor > 6 || (int)video_scale_factor == 0)
        return false;

    // Lastly -- check last byte is 0xFE (end code!)
    if (video_bin[video_bin_len - 1] != 0xFE)
        return false;
    
    // LGTM!
    return true;
}

typedef struct {
    int x;
    int y;
    int x2;
    int y2;
} vid84rect_t;

void begin_decode(void)
{
    bool loop = true;

    // Start at 0x9 : 0x8 is always a frame start we can ignore it.
    int last_data_index = 9;
    unsigned char data;

    // Defines the time to wait per frame.
    int time_per_frame_ms = 1000/(int)video_fps;
    int frame_time;

    // heheh.
    while(loop) {
        // Define clocks and start timer
        clock_t start_time, end_time;
        start_time = clock();

        // Blank Canvas
        gfx_FillScreen(255);

        // We have a 240x240 canvas, on a 320x240 display.
        // That's 80px left over space, 40px on each side.
        // Let's add some black borders.
        gfx_FillRectangle(0, 0, 40, 240); // Left side
        gfx_FillRectangle(280, 0, 40, 240); // Right side

        // The rectangle we are going to be drawing.
        int rect_data_index = 0;
        vid84rect_t rectangle;

        // Start decoding and rendering the frame.
        bool decoding_frame = true;
        while(decoding_frame) {
            data = video_bin[last_data_index];

            // New frame or EoF
            if (data == 0xFF || data == 0xFE) {
                decoding_frame = false;
                break;
            }

            // Throw the data where it needs to be.
            switch(rect_data_index) {
                case 0: rectangle.x = (int)data * (int)video_scale_factor; break;
                case 1: rectangle.y = (int)data * (int)video_scale_factor; break;
                case 2: rectangle.x2 = (int)data * (int)video_scale_factor; break;
                case 3: rectangle.y2 = (int)data * (int)video_scale_factor; break;
                default: break;
            }

            // Increment the rect index
            rect_data_index++;

            // Time to draw it!
            if (rect_data_index >= 4) {
                // X values need moved forward 40px to be centered in the viewport
                rectangle.x += 40;
                rectangle.x2 += 40;

                int width = abs(rectangle.x2 - rectangle.x);
                int height = abs(rectangle.y2 - rectangle.y);

                // Sometimes really precise rectangles are going to return 0 values,
                // force these to one px so things are still visible.
                if (height == 0) height = (int)video_scale_factor;
                if (width == 0) width = (int)video_scale_factor;

                gfx_FillRectangle(rectangle.x, rectangle.y, width, height);

                rect_data_index = 0;
            }

            last_data_index++;
        }

        bool end_of_file = false;

        if (data == 0xFE)
            end_of_file = true;
        else
            last_data_index++;

        // Get the End time
        end_time = clock();

        // Do we need to sleep?
        frame_time = (int)(1000 * (end_time - start_time) / CLOCKS_PER_SEC);
        int sleep_time = time_per_frame_ms - frame_time;

        if (sleep_time > 0) {
            usleep(sleep_time * 1000);
        }

        if (end_of_file == true) {
            loop = false;
            break;
        }
    }
}

int main(void)
{
    // Init the graphics lib
    gfx_Begin();

    // Make sure the video is good and store its header vars
    bool valid_vid = retrieve_data_from_video();

    // Video is BORKED! Report to user and let them leave cleanly.
    if (!valid_vid) {
        gfx_PrintStringXY("== BAD VIDEO FILE ==", 5, 5);
        gfx_PrintStringXY("Try encoding again, or stop trying", 5, 15);
        gfx_PrintStringXY("to break my decoder >:((", 5, 25);
        gfx_PrintStringXY("Press any key to exit..", 5, 45);

        while (!os_GetCSC());
        gfx_End();
        return -1;
    }
    // Allow a prompt to start
    else {
        gfx_PrintStringXY("== LOADED VIDEO FILE ==", 5, 5);
        gfx_PrintStringXY("Press any key to play! :D", 5, 15);
        while (!os_GetCSC());
        begin_decode();
    }

    // Clean up
    gfx_End();
    return 0;
}