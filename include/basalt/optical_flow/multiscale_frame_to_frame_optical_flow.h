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

// Original source for multi-scale implementation:
// https://github.com/DLR-RM/granite (MIT license)

#pragma once

#include <memory>
#include <sophus/se2.hpp>

#include <tbb/blocked_range.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/parallel_for.h>

#include <basalt/optical_flow/optical_flow.h>
#include <basalt/optical_flow/patch.h>

#include <basalt/imu/preintegration.h>
#include <basalt/utils/imu_types.h>
#include <basalt/utils/keypoints.h>

namespace basalt {

/// MultiscaleFrameToFrameOpticalFlow is the same as FrameToFrameOpticalFlow,
/// but patches can be created at all pyramid levels, not just the lowest
/// pyramid.
template <typename Scalar, template <typename> typename Pattern>
class MultiscaleFrameToFrameOpticalFlow final : public OpticalFlowTyped<Scalar, Pattern> {
 public:
  using Vector3d = Eigen::Matrix<double, 3, 1>;

  using typename OpticalFlowTyped<Scalar, Pattern>::PatchT;
  using typename OpticalFlowTyped<Scalar, Pattern>::Vector2;
  using typename OpticalFlowTyped<Scalar, Pattern>::Matrix2;
  using typename OpticalFlowTyped<Scalar, Pattern>::Vector3;
  using typename OpticalFlowTyped<Scalar, Pattern>::Matrix3;
  using typename OpticalFlowTyped<Scalar, Pattern>::Vector4;
  using typename OpticalFlowTyped<Scalar, Pattern>::Matrix4;
  using typename OpticalFlowTyped<Scalar, Pattern>::SE2;
  using typename OpticalFlowTyped<Scalar, Pattern>::SE3;
  using OpticalFlowTyped<Scalar, Pattern>::getNumCams;
  using OpticalFlowTyped<Scalar, Pattern>::calib;
  using OpticalFlowTyped<Scalar, Pattern>::E;

  using OpticalFlowBase::config;
  using OpticalFlowBase::depth_guess;
  using OpticalFlowBase::first_state_arrived;
  using OpticalFlowBase::frame_counter;
  using OpticalFlowBase::input_depth_queue;
  using OpticalFlowBase::input_img_queue;
  using OpticalFlowBase::input_imu_queue;
  using OpticalFlowBase::input_state_queue;
  using OpticalFlowBase::last_keypoint_id;
  using OpticalFlowBase::latest_state;
  using OpticalFlowBase::old_pyramid;
  using OpticalFlowBase::output_queue;
  using OpticalFlowBase::patch_coord;
  using OpticalFlowBase::predicted_state;
  using OpticalFlowBase::processing_thread;
  using OpticalFlowBase::pyramid;
  using OpticalFlowBase::show_gui;
  using OpticalFlowBase::t_ns;
  using OpticalFlowBase::transforms;

  MultiscaleFrameToFrameOpticalFlow(const VioConfig& conf, const Calibration<double>& cal)
      : OpticalFlowTyped<Scalar, Pattern>(conf, cal),
        accel_cov(cal.dicrete_time_accel_noise_std().array().square()),
        gyro_cov(cal.dicrete_time_gyro_noise_std().array().square()) {
    latest_state = std::make_shared<PoseVelBiasState<double>>();
    predicted_state = std::make_shared<PoseVelBiasState<double>>();
  }

  void processingLoop() override {
    using std::make_shared;
    OpticalFlowInput::Ptr img;

    while (true) {
      input_img_queue.pop(img);

      if (img == nullptr) {
        if (output_queue) output_queue->push(nullptr);
        break;
      }
      img->addTime("frontend_frames_received");

      while (input_depth_queue.try_pop(depth_guess)) continue;
      if (show_gui) img->depth_guess = depth_guess;

      if (!input_state_queue.empty()) {
        while (input_state_queue.try_pop(latest_state)) continue;  // Flush
        first_state_arrived = true;
      } else if (first_state_arrived) {
        latest_state = make_shared<PoseVelBiasState<double>>(*predicted_state);
      }

      if (first_state_arrived) {
        auto pim = processImu(img->t_ns);
        pim.predictState(*latest_state, constants::g, *predicted_state);
      }

      processFrame(img->t_ns, img);
    }
  }

  IntegratedImuMeasurement<double> processImu(int64_t curr_t_ns) {
    int64_t prev_t_ns = t_ns;
    Vector3d bg = latest_state->bias_gyro;
    Vector3d ba = latest_state->bias_accel;
    IntegratedImuMeasurement<double> pim{prev_t_ns, bg, ba};

    if (input_imu_queue.empty()) return pim;

    auto pop_imu = [&](ImuData<double>::Ptr& data) -> bool {
      input_imu_queue.pop(data);  // Blocking pop
      if (data == nullptr) return false;

      // Calibrate sample
      Vector3 a = calib.calib_accel_bias.getCalibrated(data->accel.cast<Scalar>());
      Vector3 g = calib.calib_gyro_bias.getCalibrated(data->gyro.cast<Scalar>());
      data->accel = a.template cast<double>();
      data->gyro = g.template cast<double>();
      return true;
    };

    typename ImuData<double>::Ptr data = nullptr;
    if (!pop_imu(data)) return pim;

    while (data->t_ns <= prev_t_ns) {
      if (!pop_imu(data)) return pim;
    }

    while (data->t_ns <= curr_t_ns) {
      pim.integrate(*data, accel_cov, gyro_cov);
      if (!pop_imu(data)) return pim;
    }

    // Pretend last IMU sample before "now" happened now
    if (pim.get_start_t_ns() + pim.get_dt_ns() < curr_t_ns) {
      data->t_ns = curr_t_ns;
      pim.integrate(*data, accel_cov, gyro_cov);
    }

    return pim;
  }

  void processFrame(int64_t curr_t_ns, OpticalFlowInput::Ptr& new_img_vec) {
    for (const auto& v : new_img_vec->img_data) {
      if (!v.img.get()) return;
    }

    const size_t num_cams = getNumCams();

    if (t_ns < 0) {
      t_ns = curr_t_ns;

      transforms = std::make_shared<OpticalFlowResult>();
      transforms->keypoints.resize(num_cams);
      transforms->pyramid_levels.resize(num_cams);
      transforms->tracking_guesses.resize(num_cams);
      transforms->matching_guesses.resize(num_cams);
      transforms->t_ns = t_ns;

      pyramid = std::make_shared<std::vector<ManagedImagePyr<uint16_t>>>();
      pyramid->resize(num_cams);

      tbb::parallel_for(tbb::blocked_range<size_t>(0, num_cams), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          pyramid->at(i).setFromImage(*new_img_vec->img_data[i].img, config.optical_flow_levels);
        }
      });

      transforms->input_images = new_img_vec;

      addPoints();
      filterPoints();
    } else {
      t_ns = curr_t_ns;

      old_pyramid = pyramid;

      pyramid = std::make_shared<std::vector<ManagedImagePyr<uint16_t>>>();
      pyramid->resize(num_cams);
      tbb::parallel_for(tbb::blocked_range<size_t>(0, num_cams), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
          pyramid->at(i).setFromImage(*new_img_vec->img_data[i].img, config.optical_flow_levels);
        }
      });

      OpticalFlowResult::Ptr new_transforms;
      new_transforms = std::make_shared<OpticalFlowResult>();
      new_transforms->keypoints.resize(num_cams);
      new_transforms->tracking_guesses.resize(num_cams);
      new_transforms->matching_guesses.resize(num_cams);
      new_transforms->pyramid_levels.resize(num_cams);
      new_transforms->t_ns = t_ns;

      SE3 T_i1 = latest_state->T_w_i.template cast<Scalar>();
      SE3 T_i2 = predicted_state->T_w_i.template cast<Scalar>();
      for (size_t i = 0; i < num_cams; i++) {
        SE3 T_c1 = T_i1 * calib.T_i_c[i];
        SE3 T_c2 = T_i2 * calib.T_i_c[i];
        SE3 T_c1_c2 = T_c1.inverse() * T_c2;
        trackPoints(old_pyramid->at(i), pyramid->at(i),                               //
                    transforms->keypoints[i], transforms->pyramid_levels[i],          //
                    new_transforms->keypoints[i], new_transforms->pyramid_levels[i],  //
                    new_transforms->tracking_guesses[i],                              //
                    new_img_vec->masks.at(i), new_img_vec->masks.at(i), T_c1_c2, i, i);
      }

      transforms = new_transforms;
      transforms->input_images = new_img_vec;

      addPoints();
      filterPoints();
    }

    if (output_queue && frame_counter % config.optical_flow_skip_frames == 0) {
      transforms->input_images->addTime("frontend_keypoints_pushed");
      output_queue->push(transforms);
    }

    frame_counter++;
  }

  void trackPoints(const ManagedImagePyr<uint16_t>& pyr_1, const ManagedImagePyr<uint16_t>& pyr_2,  //
                   const Keypoints& keypoint_map_1, const KeypointLevels& pyramid_levels_1,         //
                   Keypoints& keypoint_map_2, KeypointLevels& pyramid_levels_2,                     //
                   Keypoints& guesses, const Masks& masks1, const Masks& masks2, const SE3& T_c1_c2, size_t cam1,
                   size_t cam2) const {
    size_t num_points = keypoint_map_1.size();

    std::vector<KeypointId> ids;
    Eigen::aligned_vector<Keypoint> init_vec;
    std::vector<size_t> pyramid_level;

    ids.reserve(num_points);
    init_vec.reserve(num_points);
    pyramid_level.reserve(num_points);

    for (const auto& [kpid, affine] : keypoint_map_1) {
      ids.push_back(kpid);
      init_vec.push_back(affine);
      pyramid_level.push_back(pyramid_levels_1.at(kpid));
    }

    tbb::concurrent_unordered_map<KeypointId, Keypoint, std::hash<KeypointId>> result, guesses_tbb;
    tbb::concurrent_unordered_map<KeypointId, size_t, std::hash<KeypointId>> pyrlvls_tbb;

    bool tracking = cam1 == cam2;
    bool matching = cam1 != cam2;
    MatchingGuessType guess_type = config.optical_flow_matching_guess_type;
    bool match_guess_uses_depth = guess_type != MatchingGuessType::SAME_PIXEL;
    const bool use_depth = tracking || (matching && match_guess_uses_depth);
    const double depth = depth_guess;

    auto compute_func = [&](const tbb::blocked_range<size_t>& range) {
      for (size_t r = range.begin(); r != range.end(); ++r) {
        const KeypointId id = ids[r];

        const Eigen::AffineCompact2f& transform_1 = init_vec[r];
        Eigen::AffineCompact2f transform_2 = transform_1;

        auto t1 = transform_1.translation();
        auto t2 = transform_2.translation();

        if (masks1.inBounds(t1.x(), t1.y())) continue;

        bool valid = true;

        Eigen::Vector2f off{0, 0};

        if (use_depth) {
          Vector2 t2_guess;
          Scalar _;
          calib.projectBetweenCams(t1, depth, t2_guess, _, T_c1_c2, cam1, cam2);
          off = t2 - t2_guess;
        }

        t2 -= off;  // This modifies transform_2

        if (show_gui) {
          guesses_tbb[id] = transform_2;
        }

        valid = t2(0) >= 0 && t2(1) >= 0 && t2(0) < pyr_2.lvl(0).w && t2(1) < pyr_2.lvl(0).h;
        if (!valid) continue;

        valid = trackPoint(pyr_1, pyr_2, transform_1, pyramid_level[r], transform_2);
        if (!valid) continue;

        if (masks2.inBounds(t2.x(), t2.y())) continue;

        Eigen::AffineCompact2f transform_1_recovered = transform_2;
        auto t1_recovered = transform_1_recovered.translation();

        t1_recovered += off;

        valid = trackPoint(pyr_2, pyr_1, transform_2, pyramid_level[r], transform_1_recovered);
        if (!valid) continue;

        Scalar dist2 = (t1 - t1_recovered).squaredNorm();

        if (dist2 < config.optical_flow_max_recovered_dist2) {
          result[id] = transform_2;
          pyrlvls_tbb[id] = pyramid_level[r];
        }
      }
    };

    tbb::blocked_range<size_t> range(0, num_points);
    tbb::parallel_for(range, compute_func);

    keypoint_map_2.clear();
    keypoint_map_2.insert(result.begin(), result.end());
    guesses.clear();
    guesses.insert(guesses_tbb.begin(), guesses_tbb.end());
    pyramid_levels_2.clear();
    pyramid_levels_2.insert(pyrlvls_tbb.begin(), pyrlvls_tbb.end());
  }

  inline bool trackPoint(const ManagedImagePyr<uint16_t>& old_pyr, const ManagedImagePyr<uint16_t>& pyr,
                         const Eigen::AffineCompact2f& old_transform, const size_t pyramid_level,
                         Eigen::AffineCompact2f& transform) const {
    bool patch_valid = true;

    transform.linear().setIdentity();

    for (int level = config.optical_flow_levels; level >= int(pyramid_level); level--) {
      const Scalar scale = 1 << unsigned(level);

      Eigen::AffineCompact2f transform_tmp = transform;

      transform_tmp.translation() /= scale;

      PatchT p(old_pyr.lvl(level), old_transform.translation() / scale);

      patch_valid &= p.valid;
      if (patch_valid) {
        // Perform tracking on current level
        patch_valid &= trackPointAtLevel(pyr.lvl(level), p, transform_tmp);
      }

      if (level == int(pyramid_level) + 1 && !patch_valid) {
        return false;
      }

      transform_tmp.translation() *= scale;

      if (patch_valid) {
        transform = transform_tmp;
      }
    }

    transform.linear() = old_transform.linear() * transform.linear();

    return patch_valid;
  }

  inline bool trackPointAtLevel(const Image<const uint16_t>& img_2, const PatchT& dp,
                                Eigen::AffineCompact2f& transform) const {
    bool patch_valid = true;

    for (int iteration = 0; patch_valid && iteration < config.optical_flow_max_iterations; iteration++) {
      typename PatchT::VectorP res;

      typename PatchT::Matrix2P transformed_pat = transform.linear().matrix() * PatchT::pattern2;
      transformed_pat.colwise() += transform.translation();

      patch_valid &= dp.residual(img_2, transformed_pat, res);

      if (patch_valid) {
        const Vector3 inc = -dp.H_se2_inv_J_se2_T * res;

        // avoid NaN in increment (leads to SE2::exp crashing)
        patch_valid &= inc.array().isFinite().all();

        // avoid very large increment
        patch_valid &= inc.template lpNorm<Eigen::Infinity>() < 1e6;

        if (patch_valid) {
          transform *= SE2::exp(inc).matrix();

          const int filter_margin = 2;

          patch_valid &= img_2.InBounds(transform.translation(), filter_margin);
        }
      }
    }

    return patch_valid;
  }

  std::pair<Keypoints, KeypointLevels> addPointsForCamera(size_t cam_id, int level) {
    Eigen::aligned_vector<Eigen::Vector2d> pts;  // Current points
    for (const auto& [kpid, affine] : transforms->keypoints.at(cam_id)) {
      const int point_level = int(transforms->pyramid_levels.at(cam_id).at(kpid));
      if (point_level >= level - 1 && point_level <= level + 1) {  // Use the point on adjacent levels too
        const Scalar pt_scale = 1 << unsigned(point_level);
        pts.emplace_back((affine.translation() / pt_scale).template cast<double>());
      }
    }

    const Scalar scale = 1 << unsigned(level);

    KeypointsData kd;  // Detected new points
    detectKeypoints(pyramid->at(cam_id).lvl(level), kd, config.optical_flow_detection_grid_size,
                    config.optical_flow_detection_num_points_cell, config.optical_flow_detection_min_threshold,
                    config.optical_flow_detection_max_threshold, config.optical_flow_image_safe_radius,
                    transforms->input_images->masks.at(cam_id), pts);

    //! @note Using static grid size for all levels. Scaling it made feature
    //! count increase 2x but ATE/RTE scores were just slightly worse/better.

    Keypoints new_kpts;
    KeypointLevels new_pyramid_levels;
    for (auto& corner : kd.corners) {  // Set new points as keypoints
      auto transform = Eigen::AffineCompact2f::Identity();
      transform.translation() = corner.cast<Scalar>() * scale;

      transforms->keypoints.at(cam_id)[last_keypoint_id] = transform;
      transforms->pyramid_levels.at(cam_id)[last_keypoint_id] = level;
      new_kpts[last_keypoint_id] = transform;
      new_pyramid_levels[last_keypoint_id] = level;

      last_keypoint_id++;
    }

    return std::make_pair(new_kpts, new_pyramid_levels);
  }

  Masks cam0OverlapCellsMasksForCam(size_t cam_id) {
    int C = config.optical_flow_detection_grid_size;  // cell size

    int w = transforms->input_images->img_data.at(cam_id).img->w;
    int h = transforms->input_images->img_data.at(cam_id).img->h;

    int x_start = (w % C) / 2;
    int y_start = (h % C) / 2;

    int x_stop = x_start + C * (w / C - 1);
    int y_stop = y_start + C * (h / C - 1);

    int x_first = x_start + C / 2;
    int y_first = y_start + C / 2;

    int x_last = x_stop + C / 2;
    int y_last = y_stop + C / 2;

    Masks masks;
    for (int y = y_first; y <= y_last; y += C) {
      for (int x = x_first; x <= x_last; x += C) {
        Vector2 ci_uv{x, y};
        Vector2 c0_uv;
        Scalar _;
        bool projected = calib.projectBetweenCams(ci_uv, depth_guess, c0_uv, _, cam_id, 0);
        bool in_bounds = c0_uv.x() >= 0 && c0_uv.x() < w && c0_uv.y() >= 0 && c0_uv.y() < h;
        bool valid = projected && in_bounds;
        if (valid) {
          Rect cell_mask(x - C / 2.0f, y - C / 2.0f, C, C);
          masks.masks.push_back(cell_mask);
        }
      }
    }
    return masks;
  }

  void addPoints() {
    //! @note Not using the smallest pyramid for some reason. When trying it
    //! ATE/RTE scores got slight worse.
    for (int level = 0; level < config.optical_flow_levels - 1; level++) {
      Masks& ms0 = transforms->input_images->masks.at(0);
      auto [kpts0, lvls0] = addPointsForCamera(0, level);

      for (size_t i = 1; i < getNumCams(); i++) {
        Masks& ms = transforms->input_images->masks.at(i);
        Keypoints& mgs = transforms->matching_guesses.at(i);

        // Match features on areas that overlap with cam0 using optical flow
        auto& pyr0 = pyramid->at(0);
        auto& pyri = pyramid->at(i);
        Keypoints kpts;
        KeypointLevels lvls;
        SE3 T_c0_ci = calib.T_i_c[0].inverse() * calib.T_i_c[i];
        trackPoints(pyr0, pyri, kpts0, lvls0, kpts, lvls, mgs, ms0, ms, T_c0_ci, 0, i);
        transforms->keypoints.at(i).insert(kpts.begin(), kpts.end());
        transforms->pyramid_levels.at(i).insert(lvls.begin(), lvls.end());
      }
    }

    // Update masks and detect features on area not overlapping with cam0
    if (!config.optical_flow_detection_nonoverlap) return;
    for (size_t i = 1; i < getNumCams(); i++) {
      Masks& ms = transforms->input_images->masks.at(i);
      ms += cam0OverlapCellsMasksForCam(i);
      for (int l = 0; l < config.optical_flow_levels - 1; l++) addPointsForCamera(i, l);
    }
  }

  void filterPointsForCam(int cam_id) {
    std::set<KeypointId> kp_to_remove;

    std::vector<KeypointId> kpids;
    Eigen::aligned_vector<Eigen::Vector2f> proj0, proj1;

    for (const auto& [kpid, affine] : transforms->keypoints.at(cam_id)) {
      auto it = transforms->keypoints.at(0).find(kpid);

      if (it != transforms->keypoints.at(0).end()) {
        proj0.emplace_back(it->second.translation());
        proj1.emplace_back(affine.translation());
        kpids.emplace_back(kpid);
      }
    }

    Eigen::aligned_vector<Eigen::Vector4f> p3d0, p3d1;
    std::vector<bool> p3d0_success, p3d1_success;

    calib.intrinsics[0].unproject(proj0, p3d0, p3d0_success);
    calib.intrinsics[cam_id].unproject(proj1, p3d1, p3d1_success);

    for (size_t i = 0; i < p3d0_success.size(); i++) {
      if (p3d0_success[i] && p3d1_success[i]) {
        const double epipolar_error = std::abs(p3d0[i].transpose() * E[cam_id] * p3d1[i]);

        const Scalar scale = 1 << transforms->pyramid_levels.at(cam_id).at(kpids[i]);
        if (epipolar_error > config.optical_flow_epipolar_error * scale) {
          kp_to_remove.emplace(kpids[i]);
        }
      } else {
        kp_to_remove.emplace(kpids[i]);
      }
    }

    for (int id : kp_to_remove) {
      transforms->keypoints.at(cam_id).erase(id);
    }
  }

  void filterPoints() {
    for (size_t i = 1; i < getNumCams(); i++) {
      filterPointsForCam(i);
    }
  }

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
 private:
  const Vector3d accel_cov;
  const Vector3d gyro_cov;
};

}  // namespace basalt
