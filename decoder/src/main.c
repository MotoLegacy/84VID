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

#define RECTANGLE_QUEUE_COUNT       32  // This is the amount of rectangles we're allowed to pre-process.

// This is a process queue for downtime between frames.
vid84rect_t queued_rectangles[RECTANGLE_QUEUE_COUNT];

void init_render_queue(void)
{
    // Set the queue to -1 so we know there's no data in it.
    for (int i = 0; i < RECTANGLE_QUEUE_COUNT; i++) {
        queued_rectangles[i].x = -1;
        queued_rectangles[i].y = -1;
        queued_rectangles[i].x2 = -1;
        queued_rectangles[i].y2 = -1;
    }
}

void process_next_frame(int* last_data_index, clock_t start_time, int time_per_frame_ms)
{
    bool time_to_process = true;
    unsigned char data;
    int frame_time;
    clock_t curr_time;

    int rect_data_index = 0;
    int rect_queue_index = 0;

    bool queue_full = false;
    bool end_of_frame = false;

    while(time_to_process) {
        // Don't do anything if the queue is full.
        if (queue_full == false && end_of_frame == false) {
            // Grab some video data
            data = video_bin[*last_data_index];

            // Not EoF or new frame indicator
            if (data != 0xFE && data != 0xFF) {

                // Store the data
                switch(rect_data_index) {
                    case 0: queued_rectangles[rect_queue_index].x = (int)data * (int)video_scale_factor; break;
                    case 1: queued_rectangles[rect_queue_index].y = (int)data * (int)video_scale_factor; break;
                    case 2: queued_rectangles[rect_queue_index].x2 = (int)data * (int)video_scale_factor; break;
                    case 3: queued_rectangles[rect_queue_index].y2 = (int)data * (int)video_scale_factor; break;
                    default: break;
                }

                rect_data_index++;

                // Move on to next rectangle
                if (rect_data_index >= 4) {
                    rect_data_index = 0;
                    rect_queue_index++;

                    if (rect_queue_index >= RECTANGLE_QUEUE_COUNT)
                        queue_full = true;
                }

                // Iterate the data index.
                (*last_data_index) += 1;
            } else {
                end_of_frame = true;
            }
        }

        // Check if we still have time to spare or if we've hit our budget.
        curr_time = clock();
        frame_time = (int)(1000 * (curr_time - start_time) / CLOCKS_PER_SEC);
        int off_time = time_per_frame_ms - frame_time;

        // We don't, clean up and leave.
        if (off_time <= 0) {
            // We didn't finish a rectangle, let the live decoder re-do that one.
            if (rect_data_index != 3) {
                (*last_data_index) -= rect_data_index;
            }

            time_to_process = false;
            break;
        }
    }
}

int prerender_first_frame()
{
    // Start at 0x9 -- 0x8 is always a frame start, we can ignore it.
    int last_data_index = 9;
    clock_t start_time = clock();
    int time_per_frame_ms = 500; // give it half a second to try and fill the queue.

    process_next_frame(&last_data_index, start_time, time_per_frame_ms);
    return last_data_index;
}

void process_rectangle_queue(void)
{
    // Iterate through the queue.
    for(int i = 0; i < RECTANGLE_QUEUE_COUNT; i++) {
        // It's a complete rectangle
        if (queued_rectangles[i].x != -1) {
            // X values need moved forward 40px to be centered in the viewport
            queued_rectangles[i].x += 40;
            queued_rectangles[i].x2 += 40;

            int width = abs(queued_rectangles[i].x2 - queued_rectangles[i].x);
            int height = abs(queued_rectangles[i].y2 - queued_rectangles[i].y);

            // Sometimes really precise rectangles are going to return 0 values,
            // force these to one px so things are still visible.
            if (height == 0) height = (int)video_scale_factor;
            if (width == 0) width = (int)video_scale_factor;

            gfx_FillRectangle(queued_rectangles[i].x, queued_rectangles[i].y, width, height);
        }
        // It's not, don't bother continuing to iterate
        else {
            break;
        }
    }

    // Reset the queue
    init_render_queue();
}

void begin_decode(int last_data_index)
{
    bool loop = true;
    unsigned char data;

    // Defines the time to wait per frame.
    int time_per_frame_ms = 1000/(int)video_fps;
    int frame_time;

    // We have a 240x240 canvas, on a 320x240 display.
    // That's 80px left over space, 40px on each side.
    // Let's add some black borders.
    gfx_FillRectangle(0, 0, 40, 240); // Left side
    gfx_FillRectangle(280, 0, 40, 240); // Right side

    // heheh.
    while(loop) {
        // Define clocks and start timer
        clock_t start_time, end_time;
        start_time = clock();

        // Blank Canvas
        gfx_SetColor(255);
        gfx_FillRectangle(40, 0, 240, 240);
        gfx_SetColor(0);

        // The rectangle we are going to be drawing.
        int rect_data_index = 0;
        vid84rect_t rectangle;

        // Start decoding and rendering the frame.
        bool decoding_frame = true;
        while(decoding_frame) {
            // If we were processing rectangles during our off-time,
            // draw them.
            if (queued_rectangles[0].x != -1)
                process_rectangle_queue();

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

        // Do we have some free time?
        frame_time = (int)(1000 * (end_time - start_time) / CLOCKS_PER_SEC);
        int off_time = time_per_frame_ms - frame_time;

        // We have off time, let's start processing the next frame.
        if (off_time > 0) {
            if (end_of_file == false)
                process_next_frame(&last_data_index, start_time, time_per_frame_ms);
            // Unless this is the last frame, then just wait.
            else
                usleep(off_time * 1000);
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
        gfx_PrintStringXY("Pre-Loading first frame..", 5, 25);
        init_render_queue();
        int data_index = prerender_first_frame();
        begin_decode(data_index);
    }

    // Clean up
    gfx_End();
    return 0;
}