miniz 3.0.2 (https://github.com/richgel999/miniz, MIT licence: see LICENSE), unmodified.
Only the simulator uses it, for inflating map pack tiles; the firmware calls the copy of
miniz's tinfl that is in the ESP32-S3's ROM.
