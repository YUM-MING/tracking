#include "cv_shim.h"

#include <cstdlib>
#include <cstring>

#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

struct CvsCapture {
    cv::VideoCapture cap;
    cv::Mat frame;             /* cvs_read가 반환하는 버퍼의 소유자 */
};

extern "C" {

CvsCapture *cvs_open(const char *source, int width, int height)
{
    CvsCapture *c = new CvsCapture();

    char *end = nullptr;
    long idx = strtol(source, &end, 10);
    bool is_index = end && *end == '\0';
    bool ok = is_index ? c->cap.open((int)idx) : c->cap.open(source);
    if (!ok) {
        delete c;
        return nullptr;
    }
    c->cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    c->cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    return c;
}

int cvs_read(CvsCapture *c, uint8_t **data, int *w, int *h, int *stride)
{
    if (!c->cap.read(c->frame) || c->frame.empty()) return 0;
    if (c->frame.type() != CV_8UC3) {
        cv::Mat tmp;
        c->frame.convertTo(tmp, CV_8UC3);
        c->frame = tmp;
    }
    *data = c->frame.data;
    *w = c->frame.cols;
    *h = c->frame.rows;
    *stride = (int)c->frame.step;
    return 1;
}

void cvs_close(CvsCapture *c)
{
    if (!c) return;
    c->cap.release();
    delete c;
}

static cv::Mat wrap(uint8_t *d, int w, int h, int stride)
{
    return cv::Mat(h, w, CV_8UC3, d, (size_t)stride);
}

void cvs_show(const char *window, const uint8_t *data, int w, int h, int stride)
{
    cv::Mat m(h, w, CV_8UC3, const_cast<uint8_t *>(data), (size_t)stride);
    cv::imshow(window, m);
}

int cvs_waitkey(int ms) { return cv::waitKey(ms); }

void cvs_destroy_all_windows(void) { cv::destroyAllWindows(); }

void cvs_rect(uint8_t *d, int w, int h, int stride,
              int x1, int y1, int x2, int y2, int b, int g, int r, int thick)
{
    cv::Mat m = wrap(d, w, h, stride);
    cv::rectangle(m, { x1, y1 }, { x2, y2 }, { (double)b, (double)g, (double)r }, thick);
}

void cvs_line(uint8_t *d, int w, int h, int stride,
              int x1, int y1, int x2, int y2, int b, int g, int r, int thick)
{
    cv::Mat m = wrap(d, w, h, stride);
    cv::line(m, { x1, y1 }, { x2, y2 }, { (double)b, (double)g, (double)r }, thick);
}

void cvs_circle(uint8_t *d, int w, int h, int stride,
                int cx, int cy, int radius, int b, int g, int r, int thick)
{
    cv::Mat m = wrap(d, w, h, stride);
    cv::circle(m, { cx, cy }, radius, { (double)b, (double)g, (double)r }, thick);
}

void cvs_text(uint8_t *d, int w, int h, int stride, const char *text,
              int x, int y, double scale, int b, int g, int r, int thick)
{
    cv::Mat m = wrap(d, w, h, stride);
    cv::putText(m, text, { x, y }, cv::FONT_HERSHEY_SIMPLEX, scale,
                { (double)b, (double)g, (double)r }, thick);
}

} /* extern "C" */
