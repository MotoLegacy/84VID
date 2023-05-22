"""84VID Video Encoder

This script allows the user to encode OpenCV-compatible video files to
84VID version 1.0 videos, specifying integer scale, framerate, and
color output thresholds.

See requirements.txt provided with this script for a list of required
modules.
"""

import argparse
import struct
import shutil
import sys
import os
import cv2
import numpy as np
from colorama import Fore, Style

args = {}

TEMP_DIR_PATH = 'temp/'
MAGIC = '84VID'
VERS = 1
COL_BLUE = Fore.BLUE
COL_RED = Fore.RED
COL_YEL = Fore.YELLOW
COL_GREEN = Fore.GREEN
COL_NONE = Style.RESET_ALL

def get_crop_boundaries_for_image():
    '''
    Returns the XYWH boundaries for cropping OpenCV finds
    using the frame number provided from -cf.
    '''
    cf = int(args['crop_frame'])

    # Report.
    print(f'{COL_BLUE}* {COL_NONE} Calculating boundaries for cropping.')

    if cf == -1:
        print(f'{COL_YEL}- {COL_NONE} Cropping disabled. This could result in wasteful meshes.')
        print('   Consider yourself warned!')
        return 0, 0, 0, 0
    
    # This is a "little" slow -- but we just open the video and interate
    # through it like we do in conversion, the reason being is we want
    # to avoid spamming the disk with writes, which we'd do if we export,
    # then crop, then re-export. :^)
    capture = cv2.VideoCapture(args['input_file'])

    i = 0

    while capture.isOpened():
        frame_read, frame = capture.read()

        if frame_read:
            if i == cf:
                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                _,thresh = cv2.threshold(gray, 1, 255, cv2.THRESH_BINARY)
                cont,hi = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
                cnt = cont[0]
                return cv2.boundingRect(cnt)
            i += 1
        else:
            break

    # We never encountered the frame, spew an error.
    print(f'{COL_RED}Error{COL_NONE}: Could not find crop frame {cf}. Video had {i} frames.')
    sys.exit()


def convert_video_to_image_sequence():
    '''
    Uses the input file argument obtained to open the video
    file and dump its frame contents as .PNG images.
    '''
    # Remove existing temp directory in case
    # there was failure before.
    if os.path.exists(TEMP_DIR_PATH):
        shutil.rmtree(TEMP_DIR_PATH)

    # Create the directory for temp image output.
    os.mkdir(TEMP_DIR_PATH)

    # Grab the input file.
    capture = cv2.VideoCapture(args['input_file'])

    # Make sure target framerate isn't higher than
    # our video
    vid_fps = int(capture.get(cv2.CAP_PROP_FPS))
    fps = int(args['fps'])

    # It is, warn and set it to the video's.
    if fps > vid_fps:
        print(f'{COL_YEL}- {COL_NONE} Desired FPS ({fps}) is higher than video ({vid_fps}).')
        print('   Force-setting to video framerate instead.')
        args['fps'] = vid_fps

    # Additionally, get the ratio of FPS rate for cutting frames.
    fps_rate = vid_fps / fps

    # Retrieve resolution scale
    res = int(240 / int(args['scale_factor']))

    # Report Status
    print(f'{COL_BLUE}* {COL_NONE} Starting image sequencing.')

    i = 0
    frame_caps = 0

    # Image crop boundaries
    crop_x, crop_y, crop_w, crop_h = get_crop_boundaries_for_image()

    # Dump the video as images.
    while capture.isOpened():
        frame_read, frame = capture.read()

        if frame_read:
            frame_caps = frame_caps + 1

            # Check if we should capture this frame.
            if frame_caps % fps_rate != 0:
                continue

            path = f'{TEMP_DIR_PATH}frame{str(i)}.png'

            if (crop_x != 0 and crop_y != 0) or (crop_w != 0 and crop_h != 0):
                frame = frame[crop_y:crop_y+crop_h,crop_x:crop_x+crop_w]

            # Resize to resolution boundaries and export.
            frame = cv2.resize(frame, (res, res))
            cv2.imwrite(path, frame)
            i += 1
        else:
            print(f'{COL_BLUE}* {COL_NONE} Converted video to image sequence.')
            break

    if i == 0:
        print(f'{COL_RED}Error{COL_NONE}: No frames generated, bad video input.')
        sys.exit()

    return i - 1

def image_to_array(image):
    '''
    Takes in an OpenCV image and converts it into a 2D Array
    whose contents are either '1' (for a black pixel) or '0'
    (for a while pixel) at coordinates [X][Y].
    '''
    # Retrieve resolution scale
    res = int(240 / int(args['scale_factor']))

    # Start with all black pixels
    array = [[1 for i in range(res)] for j in range(res)]

    # Retrieve color threshold
    col = int(args['color_threshold'])

    for x_coord in range(res):
        for y_coord in range(res):
            # Get the RGB color values
            col_r, col_g, col_b = (image[x_coord, y_coord])

            # If a value is above the treshold, this
            # is a filled pixel in our format.
            if col_r >= col or col_g >= col or col_b >= col:
                array[x_coord][y_coord] = 0

    return array

def greedy_mesh_frame(array):
    '''
    Takes in a 2D Array of pixel contents and walks through
    it to find potential collections of pixels that can be
    turned into rectangles using Greedy Meshing. Returns a 
    list of rectangle vertices.
    '''
    height = len(array)
    width = len(array[0])
    visited = np.zeros((height, width), dtype=bool)
    rectangles = []

    # Walk through the possible-rectangle to confirm
    # regions are valid.
    def is_valid_rectangle(top, left, bottom, right):
        for i in range(left, right + 1):
            for j in range(top, bottom + 1):
                if array[i][j] == 0 or visited[i][j]:
                    return False
        return True

    # Start expanding and exploring a region of the
    # frame array.
    def explore_rectangle(top, left):
        bottom = top
        while bottom < height and array[bottom][left] == 1 and not visited[bottom][left]:
            right = left
            while right < width and array[bottom][right] == 1 and not visited[bottom][right]:
                visited[bottom][right] = True
                right += 1
            bottom += 1
            if not is_valid_rectangle(top, left, bottom - 1, right - 1):
                break
        rectangles.append((left, top, right - 1, bottom - 1))

    # Walk through the 2D array to find pixels
    # to try and create rectangles from.
    for i in range(height):
        for j in range(width):
            if array[i][j] == 1 and not visited[i][j]:
                explore_rectangle(i, j)

    return rectangles

def push_rectangle_to_file(rect, outfile):
    '''
    Helper method to cleanly take rectangle vertices, convert
    them to bytes, and push them to the provided encoded file.
    '''
    rect_x = rect[0].to_bytes(1, byteorder='big')
    rect_y = rect[1].to_bytes(1, byteorder='big')
    rect_x2 = rect[2].to_bytes(1, byteorder='big')
    rect_y2 = rect[3].to_bytes(1, byteorder='big')

    outfile.write(rect_x)
    outfile.write(rect_y)
    outfile.write(rect_x2)
    outfile.write(rect_y2)

def encode_images_to_84vid(frames):
    '''
    Creates the encoded file, and walks through all of our
    .PNG frames to go through multiple conversion steps and
    properly write the contents into said file.
    '''
    with open(args['output_file'], 'wb') as output:
        # Generate and write the Header
        head = struct.pack('5sBBB', bytes(MAGIC, encoding='utf-8'), int(args['fps']), VERS,
        int(args['scale_factor']))
        output.write(head)

        # Walk through all of the generated frames
        i = 0
        last_percent = 0
        while i <= frames:
            path = f'{TEMP_DIR_PATH}/frame{str(i)}.png'

            # End if we're out of frames to process.
            if not os.path.isfile(path):
                print(f'{COL_RED}Error{COL_NONE}: Could not find frame {str(i)}.')
                break

            # Percentage status report
            percent = int(100 * i/(frames - 1))

            if percent % 10 == 0 and last_percent != percent:
                print(f'{COL_BLUE}* {COL_NONE} Processing at {percent}%..')
                last_percent = percent

            # First, get the image in OpenCV
            image_frame = cv2.imread(path)
            # Then turn it into a 2D array
            frame_array = image_to_array(image_frame)
            # Now a list of Greedy-Meshed rectangles
            greedy_frame = greedy_mesh_frame(frame_array)

            # Now we can begin writing the frames,
            # starting with the new frame identifier.
            output.write(b'\xFF')

            # Grab all of our rectangles
            for rectangle in greedy_frame:
                # Push 'em!
                push_rectangle_to_file(rectangle, output)

            i += 1

        # Report end of file and close it.
        print(f'{COL_BLUE}* {COL_NONE} Finished frame processing.')
        output.write(b'\xFE')
        output.close()

def fetch_cli_arguments():
    '''
    Initiates ArgParser with all potential command line arguments.
    '''
    global args
    parser = argparse.ArgumentParser(description='Encoder for 84VID \'codec\' version 1 (one).')
    parser.add_argument('-i', '--input-file',
                        help='OpenCV-supported input video file.', required=True)
    parser.add_argument('-o', '--output-file',
                        help='File name for generated 84VID file.', default='video.bin')
    parser.add_argument('-f', '--fps',
                        help='Desired output framerate for 84VID file.', default=30)
    parser.add_argument('-s', '--scale-factor',
                        help='Integer scale/divide factor for video resolution',
                        default=1)
    parser.add_argument('-ct', '--color-threshold',
                        help='0-255 color value threshold for white pixels', default=150)
    parser.add_argument('-cf', '--crop-frame',
                        help='Frame to use as reference for cropping. -1 for none.', default=0)
    args = vars(parser.parse_args())

def print_banner():
    '''
    Prints a cute banner :^)
    '''
    print('==================')
    print('84VID ENCODER V1.0')
    print('==================')

def main():
    '''
    Goes through the list of actions required to successfully
    encode an OpenCV-compatible video file into an 84VID 1.0
    encoded video.
    '''
    fetch_cli_arguments()
    print_banner()

    if not os.path.isfile(args['input_file']):
        print(f'{COL_RED}Error{COL_NONE}: Input video file does not exist. Exiting.')
        sys.exit()

    frames = convert_video_to_image_sequence()
    encode_images_to_84vid(frames)

    print(f'{COL_GREEN}Done!{COL_NONE} 😃')
    sys.exit()


if __name__ == '__main__':
    main()
