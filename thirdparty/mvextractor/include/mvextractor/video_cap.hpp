#pragma once
#include <chrono>
#include <iostream>

// FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/motion_vector.h>
#include <libswscale/swscale.h>
}
// for changing the dtype of motion vector
#define MVS_DTYPE int32_t
#define MVS_DTYPE_NP NPY_INT32

struct Image_FFMPEG {
  unsigned char *data;
  int width;
  int height;
  int step;
  int cn;
};

class VideoCap {
 private:
  const char *url;
  AVDictionary *opts;
  const AVCodec *codec;
  AVCodecParameters *codec_params;
  AVFormatContext *fmt_ctx;
  AVCodecContext *video_dec_ctx;
  AVStream *video_stream;
  int video_stream_idx;
  AVPacket *packet;
  AVFrame *frame;
  AVFrame rgb_frame;
  Image_FFMPEG picture;
  struct SwsContext *img_convert_ctx;
  int64_t frame_number;
  double frame_timestamp;
  bool is_rtsp;

 public:
  VideoCap();

  void release();

  int open(const char *url);

  int grab();

  /** Decodes and returns the grabbed frame and motion vectors
   *
   * @param frame Pointer to the raw data of the decoded video frame. The
   *    frame is stored as a C contiguous array of shape (height, width, 3) and
   *    can be converted into a cv::Mat by using the constructor
   *    `cv::Mat cv_frame(height, width, CV_MAKETYPE(CV_8U, 3), frame)`.
   *    Note: A subsequent call of `retrieve` will reuse the same memory for
   *          storing the new frame. If you want a frame to persist for a longer
   *          perdiod of time, allocate a new array and memcopy the raw frame
   *          data into it. After usage you have to manually free this copied
   *          array.
   *
   * @param width Width of the returned frame in pixels.
   *
   * @param height Height of the returned frame in pixels.
   *
   * @param frame_type Either "P", "B" or "I" indicating whether it is an
   *    intra-coded frame (I), a predicted frame with only references to past
   *    frames (P) or reference to both past and future frames (B). Motion
   *    vectors are only returned for "P" and "B" frames.
   *
   * @param motion_vectors Pointer to the raw data of the motion vectors
   *    belonging to the decoded frame. The motion vectors are stored as a
   *    C contiguous array of shape (num_mvs, 10). Each row of the array
   *    corresponds to one motion vector. The columns of each vector have the
   *    following meaning (also refer to AVMotionVector in FFMPEG
   *    documentation):
   *    - 0: source: Where the current macroblock comes from. Negative value
   *                 when it comes from the past, positive value when it comes
   *                 from the future.
   *    - 1: w: Width and height of the vector's macroblock.
   *    - 2: h: Height of the vector's macroblock.
   *    - 3: src_x: x-location of the vector's origin in source frame (in pixels).
   *    - 4: src_y: y-location of the vector's origin in source frame (in pixels).
   *    - 5: dst_x: x-location of the vector's destination in the current frame
   *                (in pixels).
   *    - 6: dst_y: y-location of the vector's destination in the current frame
   *                (in pixels).
   *    - 7: motion_x: src_x = dst_x + motion_x / motion_scale
   *    - 8: motion_y: src_y = dst_y + motion_y / motion_scale
   *    - 9: motion_scale: see definiton of columns 7 and 8
   *    Note: If no motion vectors are present in a frame, e.g. if the frame is
   *          an I frame, `num_mvs` will be 0 and no memory is allocated for
   *          the motion vectors.
   *    Note: Other than the frame array, new memory for storing motion vectors
   *          is allocated on every call of `retrieve`, thus memcopying is not
   *          needed to persist the motion vectors for a longer period of time.
   *          Note, that the buffer needs to be freed manually by calling
   *          `free(motion_vectors)` when the motion vectors are not needed
   *          anymore.
   *
   * @param num_mvs The number of motion vectors corresponding to the rows of
   *    the motion vector array.
   *
   * @param frame_timestamp UTC wall time of each frame in the format of a UNIX
   *    timestamp. In case, input is a video file, the timestamp is derived
   *    from the system time. If the input is an RTSP stream the timestamp
   *    marks the time the frame was sent out by the sender (e.g. IP camera).
   *    Thus, the timestamp represents the wall time at which the frame was
   *    taken rather then the time at which the frame was received. This allows
   *    e.g. for accurate synchronization of multiple RTSP streams. In order
   *    for this to work, the RTSP sender needs to generate RTCP sender
   *    reports which contain a mapping from wall time to stream time. Not all
   *    RTSP senders will send sender reports as it is not part of the
   *    standard. If IP cameras are used which implement the ONVIF standard,
   *    sender reports are always sent and thus timestamps can always be
   *    computed.
   *
   * @retval true if the grabbed video frame and motion vectors could be
   *    decoded and returned successfully, false otherwise.
   */
  int retrieve(uint8_t **frame, int *step, int *width, int *height, int *cn, char *frame_type,
               MVS_DTYPE **motion_vectors, MVS_DTYPE *num_mvs, double *frame_timestamp);

  /** Convenience wrapper which combines a call of `grab` and `retrieve`.
   *
   *   The parameters and return value correspond to the `retrieve` method.
   */
  int read(uint8_t **frame, int *step, int *width, int *height, int *cn, char *frame_type, MVS_DTYPE **motion_vectors,
           MVS_DTYPE *num_mvs, double *frame_timestamp);
};
