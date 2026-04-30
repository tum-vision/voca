/**
BSD 3-Clause License

This file is part of the Basalt project.
https://gitlab.com/VladyslavUsenko/basalt.git

Copyright (c) 2019, Vladyslav Usenko and Nikolaus Demmel.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef DATASET_IO_EUROC_H
#define DATASET_IO_EUROC_H

#include <memory>

#include <basalt/io/dataset_io.h>
#include <basalt/utils/filesystem.h>
#include <libavutil/error.h>
#include <mvextractor/video_cap.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <filesystem>
#include <iterator>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/videoio.hpp>
#include <string>

namespace basalt {

class EurocVioDataset : public VioDataset {
  size_t num_cams;

  std::string path;

  std::vector<int64_t> image_timestamps;
  std::unordered_map<int64_t, std::string> image_path;
  std::vector<cv::VideoCapture> caps;
  std::vector<VideoCap> mv_caps;
  size_t frameNumber = 0;
  size_t mv_frame_counter = 0;

  std::set<size_t> mv_warning_shown_cams;

  // vector of images for every timestamp
  // assumes vectors size is num_cams for every timestamp with null pointers for
  // missing frames
  // std::unordered_map<int64_t, std::vector<ImageData>> image_data;

  Eigen::aligned_vector<AccelData> accel_data;
  Eigen::aligned_vector<GyroData> gyro_data;

  std::vector<int64_t> gt_timestamps;                // ordered gt timestamps
  Eigen::aligned_vector<Sophus::SE3d> gt_pose_data;  // TODO: change to eigen aligned

  int64_t mocap_to_imu_offset_ns = 0;

  std::vector<std::unordered_map<int64_t, double>> exposure_times;

 public:
  ~EurocVioDataset() override {
    for (cv::VideoCapture& cap : caps) {
      cap.release();
    }
    for (VideoCap& mv_cap : mv_caps) {
      mv_cap.release();
    }
  }

  size_t get_num_cams() const override { return num_cams; }

  std::vector<int64_t> &get_image_timestamps() override { return image_timestamps; }

  const Eigen::aligned_vector<AccelData> &get_accel_data() const override { return accel_data; }
  const Eigen::aligned_vector<GyroData> &get_gyro_data() const override { return gyro_data; }
  const std::vector<int64_t> &get_gt_timestamps() const override { return gt_timestamps; }
  const Eigen::aligned_vector<Sophus::SE3d> &get_gt_pose_data() const override { return gt_pose_data; }

  int64_t get_mocap_to_imu_offset_ns() const override { return mocap_to_imu_offset_ns; }

  std::vector<ImageData> get_image_data(int64_t t_ns) override {
    std::vector<ImageData> res(num_cams);
    // The dataset is first read (and caps are opened for every video file found)
    // and after that the use_mvs flag is set on the dataset object making this necessary
    if (!use_mvs && mv_caps.size() > 0) {
      //			std::cout << "NOT using motion vectors for tracking guesses!" << std::endl;
      for (VideoCap& mv_cap : mv_caps) {
        mv_cap.release();
      }
      mv_caps.clear();
    }

    const std::string video_root = video_dataset_path.empty() ? path : video_dataset_path;

    // Open RGB video captures lazily so an optional video_dataset_path can be
    // set after read() and still be respected.
    if (caps.empty()) {
      for (size_t cam_idx = 0; cam_idx < num_cams; cam_idx++) {
        std::string video_path = video_root + "/mav0/cam" + std::to_string(cam_idx) + "/data.mp4";
        if (fs::exists(video_path)) {
          cv::VideoCapture cap{video_path};
          caps.push_back(cap);
          if (!cap.isOpened()) {
            std::cerr << "ERROR! Unable to open camera at " << video_path << std::endl;
            std::abort();
          }
        }
      }
    }

    // Open MV captures only when explicitly enabled.
    if (use_mvs && mv_caps.empty()) {
      for (size_t cam_idx = 0; cam_idx < num_cams; cam_idx++) {
        std::string video_path = video_root + "/mav0/cam" + std::to_string(cam_idx) + "/data.mp4";
        if (fs::exists(video_path)) {
          VideoCap mv_cap{};
          if (mv_cap.open(video_path.c_str()) >= 0) {
            mv_caps.push_back(mv_cap);
          } else {
            std::cerr << "ERROR! Unable to open camera for Motionvector extraction at " << video_path << std::endl;
            std::abort();
          }
        }
      }
    }

    for (size_t i = 0; i < num_cams; i++) {
      //			std::cout << "getting image data of cam " << i << std::endl;
      std::string full_path = path + "/mav0/cam" + std::to_string(i);
      std::string full_video_path = video_root + "/mav0/cam" + std::to_string(i) + "/data.mp4";
      std::string full_image_path = full_path + "/data/" + image_path[t_ns];
      cv::Mat img;

      if (use_mvs && !fs::exists(full_video_path) && mv_warning_shown_cams.count(i) == 0) {
        std::cout << "WARNING: --use-mvs is enabled but no video file found at "
                  << full_video_path << ". Motion vectors will not be available for cam" << i
                  << ". To use motion vectors, encode your image sequences into data.mp4 "
                  << "with the provided ffmpeg command." << std::endl;
        mv_warning_shown_cams.insert(i);
      }

      //&& image_timestamps[frameNumber] == t_ns
      if (use_mvs && mv_caps.size() > 0 && fs::exists(full_video_path)) {
        int ret = 0;
        //				std::cout << "Trying to read motion vectors of frame (num: " << frameNumber <<
        //") at timestamp " << t_ns << " on cam " << i << "." << std::endl;
        ret = mv_caps[i].grab();
        if (ret < 0) {
          if (ret == AVERROR(EOF) || ret == AVERROR_EOF || ret == -2) {
            std::cout << "no more frames to read here" << std::endl;
          } else {
            std::cerr << "Failed to grab motion vectors of frame (num: " << frameNumber << ") at timestamp " << t_ns
                      << "." << std::endl;
            std::abort();
          }
        } else {
          uint8_t *frame = nullptr;
          int step = 0;
          int width = 0;
          int height = 0;
          int cn = 0;
          char *frame_type = new char[2]();
          MVS_DTYPE *motion_vectors = nullptr;
          MVS_DTYPE num_mvs = 0;
          double frame_timestamp = 0;
          ret = (mv_caps[i].retrieve(&frame, &step, &width, &height, &cn, frame_type, &motion_vectors, &num_mvs,
                                     &frame_timestamp));
          if (ret < 0) {
            if (ret == AVERROR(EOF) || ret == AVERROR_EOF) {
              std::cout << "no more frames to read here" << std::endl;
            } else {
              std::cerr << "Failed to read motion vectors of frame (num: " << frameNumber << ") at timestamp " << t_ns
                        << " with response " << ret << "." << std::endl;
              std::abort();
            }
          } else {
            //						std::cout << "Successfully read " << num_mvs << " motion vectors
            // of frame (num: " << frameNumber << ") at timestamp " << t_ns << "." << std::endl;
            res.at(i).motion_vectors.reserve(num_mvs);
            for (MVS_DTYPE j = 0; j < num_mvs * 10; j = j + 10) {
              int source = motion_vectors[j];
              int bw = motion_vectors[j + 1];
              int bh = motion_vectors[j + 2];
              float sx = motion_vectors[j + 3];
              float sy = motion_vectors[j + 4];
              float dx = motion_vectors[j + 5];
              float dy = motion_vectors[j + 6];
              res.at(i).motion_vectors.push_back({source, bw, bh, sx, sy, dx, dy});
            }
            if (num_mvs > 0) {
              mv_frame_counter++;
            }
          }
        }
        // TODO: check if this releasing works properly
        if (frameNumber >= image_timestamps.size() - 1) {
          std::cout << "clearing motionvector captures after reading mvs of " << mv_frame_counter / 2
                    << " frames from a total " << frameNumber << " of frames!" << std::endl;
          mv_caps.clear();
        }
      }

      if (!use_video_frames) {
        if (!fs::exists(full_image_path)) {
          std::cerr << "Image file not found while --use-video-frames is disabled: " << full_image_path << std::endl;
          std::abort();
        }
        img = cv::imread(full_image_path, cv::IMREAD_UNCHANGED);
      } else {
        if (!(caps.size() > 0 && fs::exists(full_video_path))) {
          std::cerr << "Video frame requested but no video file found at " << full_video_path
                    << ". Provide --video-dataset-path and ensure data.mp4 exists." << std::endl;
          std::abort();
        }
        if (image_timestamps[frameNumber] != t_ns) {
          auto lower = std::lower_bound(image_timestamps.begin(), image_timestamps.end(), t_ns);
          frameNumber = std::distance(image_timestamps.begin(), lower);
        }

        if (caps[i].get(cv::CAP_PROP_POS_FRAMES) != frameNumber) {
          caps[i].set(cv::CAP_PROP_POS_FRAMES, frameNumber);
        }

        if (!caps[i].read(img)) {
          std::cerr << "Failed to read frame (num: " << frameNumber << ") at timestamp " << t_ns << "." << std::endl;
          std::abort();
        }
        frameNumber++;
      }

      if (img.empty()) {
        std::cout << "Failed to load image!" << std::endl;
        return {};
      } else if (img.type() == CV_8UC1) {
        res[i].img.reset(new ManagedImage<uint16_t>(img.cols, img.rows));

        const uint8_t *data_in = img.ptr();
        uint16_t *data_out = res[i].img->ptr;

        size_t full_size = img.cols * img.rows;
        for (size_t i = 0; i < full_size; i++) {
          int val = data_in[i];
          val = val << 8;
          data_out[i] = val;
        }
      } else if (img.type() == CV_8UC3) {
        res[i].img.reset(new ManagedImage<uint16_t>(img.cols, img.rows));

        const uint8_t *data_in = img.ptr();
        uint16_t *data_out = res[i].img->ptr;

        size_t full_size = img.cols * img.rows;
        for (size_t i = 0; i < full_size; i++) {
          int val = data_in[i * 3];
          val = val << 8;
          data_out[i] = val;
        }
      } else if (img.type() == CV_16UC1) {
        res[i].img.reset(new ManagedImage<uint16_t>(img.cols, img.rows));
        std::memcpy(res[i].img->ptr, img.ptr(), img.cols * img.rows * sizeof(uint16_t));

      } else {
        std::cerr << "img.fmt.bpp " << img.type() << std::endl;
        std::abort();
      }

      auto exp_it = exposure_times[i].find(t_ns);
      if (exp_it != exposure_times[i].end()) {
        res[i].exposure = exp_it->second;
      }
    }

    return res;
  }

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  friend class EurocIO;
};

class EurocIO : public DatasetIoInterface {
 public:
  EurocIO(bool load_mocap_as_gt) : load_mocap_as_gt(load_mocap_as_gt) {}

  void read(const std::string &path) override {
    if (!fs::exists(path)) std::cerr << "No dataset found in " << path << std::endl;

    data = std::make_shared<EurocVioDataset>();

    // Detect camera count
    int i = 0;
    while (fs::exists(path + "/mav0/cam" + std::to_string(i))) {
      i++;
    }

    data->num_cams = i;
    data->path = path;

    read_image_timestamps(path + "/mav0/cam0/");

    read_imu_data(path + "/mav0/imu0/");

    if (!load_mocap_as_gt && fs::exists(path + "/mav0/state_groundtruth_estimate0/data.csv")) {
      read_gt_data_state(path + "/mav0/state_groundtruth_estimate0/");
    } else if (!load_mocap_as_gt && fs::exists(path + "/mav0/gt/data.csv")) {
      read_gt_data_pose(path + "/mav0/gt/");
    } else if (fs::exists(path + "/mav0/mocap0/data.csv")) {
      read_gt_data_pose(path + "/mav0/mocap0/");
    }

    data->exposure_times.resize(data->num_cams);
    for (size_t i = 0; i < data->num_cams; i++) {
      std::string istr = std::to_string(i);
      std::string cam_dir = path;
      cam_dir.append("/mav0/cam").append(istr).append("/");
      if (fs::exists(cam_dir + "exposure.csv")) {
        std::cout << "Loading exposure times for cam" << istr << std::endl;
        read_exposure(cam_dir, data->exposure_times[i]);
      }
    }
  }

  void reset() override { data.reset(); }

  VioDatasetPtr get_data() override { return data; }

 private:
  void read_exposure(const std::string &path, std::unordered_map<int64_t, double> &exposure_data) {
    exposure_data.clear();

    std::ifstream f(path + "exposure.csv");
    std::string line;
    while (std::getline(f, line)) {
      if (line[0] == '#') continue;

      std::stringstream ss(line);

      char tmp;
      int64_t timestamp, exposure_int;
      Eigen::Vector3d gyro, accel;

      ss >> timestamp >> tmp >> exposure_int;

      exposure_data[timestamp] = exposure_int * 1e-9;
    }
  }

  void read_image_timestamps(const std::string &path) {
    std::ifstream f(path + "data.csv");
    std::string line;
    while (std::getline(f, line)) {
      if (line[0] == '#') continue;
      std::stringstream ss(line);
      char tmp;
      int64_t t_ns;
      std::string path;
      ss >> t_ns >> tmp >> path;

      data->image_timestamps.emplace_back(t_ns);
      data->image_path[t_ns] = path;
    }
  }

  void read_imu_data(const std::string &path) {
    data->accel_data.clear();
    data->gyro_data.clear();

    std::ifstream f(path + "data.csv");
    std::string line;
    while (std::getline(f, line)) {
      if (line[0] == '#') continue;

      std::stringstream ss(line);

      char tmp;
      uint64_t timestamp;
      Eigen::Vector3d gyro, accel;

      ss >> timestamp >> tmp >> gyro[0] >> tmp >> gyro[1] >> tmp >> gyro[2] >> tmp >> accel[0] >> tmp >> accel[1] >>
          tmp >> accel[2];

      data->accel_data.emplace_back();
      data->accel_data.back().timestamp_ns = timestamp;
      data->accel_data.back().data = accel;

      data->gyro_data.emplace_back();
      data->gyro_data.back().timestamp_ns = timestamp;
      data->gyro_data.back().data = gyro;
    }
  }

  void read_gt_data_state(const std::string &path) {
    data->gt_timestamps.clear();
    data->gt_pose_data.clear();

    std::ifstream f(path + "data.csv");
    std::string line;
    while (std::getline(f, line)) {
      if (line[0] == '#') continue;

      std::stringstream ss(line);

      char tmp;
      uint64_t timestamp;
      Eigen::Quaterniond q;
      Eigen::Vector3d pos, vel, accel_bias, gyro_bias;

      ss >> timestamp >> tmp >> pos[0] >> tmp >> pos[1] >> tmp >> pos[2] >> tmp >> q.w() >> tmp >> q.x() >> tmp >>
          q.y() >> tmp >> q.z() >> tmp >> vel[0] >> tmp >> vel[1] >> tmp >> vel[2] >> tmp >> accel_bias[0] >> tmp >>
          accel_bias[1] >> tmp >> accel_bias[2] >> tmp >> gyro_bias[0] >> tmp >> gyro_bias[1] >> tmp >> gyro_bias[2];

      data->gt_timestamps.emplace_back(timestamp);
      data->gt_pose_data.emplace_back(q, pos);
    }
  }

  void read_gt_data_pose(const std::string &path) {
    data->gt_timestamps.clear();
    data->gt_pose_data.clear();

    std::ifstream f(path + "data.csv");
    std::string line;
    while (std::getline(f, line)) {
      if (line[0] == '#') continue;

      std::stringstream ss(line);

      char tmp;
      uint64_t timestamp;
      Eigen::Quaterniond q;
      Eigen::Vector3d pos;

      ss >> timestamp >> tmp >> pos[0] >> tmp >> pos[1] >> tmp >> pos[2] >> tmp >> q.w() >> tmp >> q.x() >> tmp >>
          q.y() >> tmp >> q.z();

      data->gt_timestamps.emplace_back(timestamp);
      data->gt_pose_data.emplace_back(q, pos);
    }
  }

  std::shared_ptr<EurocVioDataset> data;
  bool load_mocap_as_gt;
};  // namespace basalt

}  // namespace basalt

#endif  // DATASET_IO_H
