#include "BallAnalysis.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// To communicate with the Python websocket server
// we use a named pipe (FIFO) stored at
//     /tmp/foos-debug.in
// These includes allow writing to such an object
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Constants
static float goalWidth = 0.15f;
static float goalHeight = 0.4f;


// Ball history
static int historyCount = 120;
static POINT balls[120];
static int ballFrames[120];
static int ballCur = 0;

static int frameNumber = 0;

static FIELD field;

static int ballMissing = 1000;

static int analysis_send_to_server(const char* str) {
    int fd = open("/tmp/foos-debug.in", O_WRONLY | O_NONBLOCK);
    if (fd > 0) {
        write(fd, str, strlen(str));
        close(fd);
        return 1;
    }
    return 0;
}

static int timeseriesfile = 0;

int analysis_init() {
    field.xmin = -0.8f;
    field.xmax =  0.8f;
    field.ymin = -0.8f;
    field.ymax =  0.8f;

#ifdef GENERATE_TIMESERIES
    timeseriesfile = open("/tmp/timeseries.txt", O_WRONLY);
    if (!timeseriesfile) {
        printf("Unable to open /tmp/timeseries.txt\n");
    } else {
        write(timeseriesfile, "(* framenumber, x, y *)\n", 24);
    }
#endif
    return 1;
}

int analysis_update(FIELD newField, POINT ball, int ballFound) {
    ++frameNumber;

    // Time average for field, because it fluctuates too much
    field.xmin = 0.98f * field.xmin + 0.02 * newField.xmin;
    field.xmax = 0.98f * field.xmax + 0.02 * newField.xmax;
    field.ymin = 0.98f * field.ymin + 0.02 * newField.ymin;
    field.ymax = 0.98f * field.ymax + 0.02 * newField.ymax;

    if (ballFound) {
        ballMissing = 0;

        balls[ballCur] = ball;
        ballFrames[ballCur] = frameNumber;
        ++ballCur;

        if(ballCur >= historyCount) {
            ballCur = 0;

            if (timeseriesfile) {
                char buffer[128];
                for (int i = 0; i < historyCount; ++i) {
                    int len = sprintf(buffer, "{%d, %f, %f},\n", ballFrames[i], balls[i].x, balls[i].y);
                    write(timeseriesfile, buffer, len);
                }
            }
        }
    } else {
        if (ballMissing++ == 30) {
            printf("Ball gone for 30 frames.\n");
            int i = ballCur - 1;
            if (i < 0) i = historyCount - 1;
            POINT lastBall = balls[i];
            float yAvg = 0.5f * (field.ymin + field.ymax);
            if (lastBall.y > yAvg - goalHeight && lastBall.y < yAvg + goalHeight) {
                if (lastBall.x < field.xmin + goalWidth) {
                    printf("-------------------------------\n");
                    printf("-------- Goal for red! --------\n");
                    printf("-------------------------------\n");
                    analysis_send_to_server("RG\n");
                }
                if (lastBall.x > field.xmax - goalWidth) {
                    printf("--------------------------------\n");
                    printf("-------- Goal for blue! --------\n");
                    printf("--------------------------------\n");
                    analysis_send_to_server("BG\n");
                }
            }
        }	

    }
    return 1;
}

// From BalltrackCore
void draw_square(float xmin, float xmax, float ymin, float ymax, uint32_t color);
void draw_line_strip(POINT* xys, int count, uint32_t color);

int analysis_draw() {
    // Draw green bounding box
    draw_square(field.xmin, field.xmax, field.ymin, field.ymax, 0xff00ff00);
    float yAvg = 0.5f * (field.ymin + field.ymax);

    // Draw `goals`
    draw_square(field.xmin, field.xmin + goalWidth, yAvg - goalHeight, yAvg + goalHeight, 0xff00ff00);
    draw_square(field.xmax - goalWidth, field.xmax, yAvg - goalHeight, yAvg + goalHeight, 0xff00ff00);

    // Draw line for ball history
    // Be carefull with circular buffer
    draw_line_strip(&balls[0], ballCur, 0xffff0000);
    draw_line_strip(&balls[ballCur], historyCount - ballCur, 0xffff0000);

    // Draw squares on detection points
    for (int i = ballCur - 20; i < ballCur; ++i) {
        int time = ballCur - i;
        int blue = 0xff - time;
        // The bytes are R,G,B,A but little-endian so 0xAABBGGRR
        int color = 0xff000000 | (blue << 16);
        float size = 0.002f * time;

        int idx = (i < 0 ? i + historyCount : i);
        POINT* pt = &balls[idx];
        draw_square(pt->x - 0.5f * size, pt->x + 0.5f * size, pt->y - size, pt->y + size, color);
    }

    return 1;
}

