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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include <magic_enum.hpp>

#include <sophus/se3.hpp>

#include <Eigen/src/Core/Matrix.h>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/global_control.h>

#include <pangolin/display/image_view.h>
#include <pangolin/gl/gldraw.h>
#include <pangolin/image/image.h>
#include <pangolin/image/image_io.h>
#include <pangolin/image/typed_image.h>
#include <pangolin/pangolin.h>
#include <pangolin/plot/datalog.h>

#include <CLI/CLI.hpp>

#include <basalt/io/dataset_io.h>
#include <basalt/io/marg_data_io.h>
#include <basalt/spline/se3_spline.h>
#include <basalt/utils/assert.h>
#include <basalt/vi_estimator/vio_estimator.h>
#include <basalt/calibration/calibration.hpp>

#include <basalt/serialization/headers_serialization.h>

#include <basalt/utils/keypoints.h>
#include <basalt/utils/system_utils.h>
#include <basalt/utils/vio_config.h>
#include <basalt/utils/vis_matrices.h>
#include <basalt/utils/vis_utils.h>
#include <basalt/linearization/landmark_block.hpp>
#include <basalt/utils/format.hpp>
#include <basalt/utils/time_utils.hpp>

// enable the "..."_format(...) string literal
using namespace basalt::literals;
using namespace basalt;
using namespace Eigen;
using pangolin::DataLog;
using pangolin::META_FLAG_READONLY;
using pangolin::Plotter;
using pangolin::Var;
using pangolin::View;
using Sophus::SE3f;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::thread;
using vis::AutoScaleDataLog;
using vis::Button;
using UIMAT = vis::UIMAT;

namespace {
std::array timing_titles = {"frames_original_timestamp",
                            "frames_read_started",
                            "frames_read",
                            "frames_pushed",
                            "frontend_frames_received",
                            "frontend_preintegration_computed",
                            "frontend_pyramid_created",
                            "frontend_tracking_ended",
                            "frontend_recall_ended",
                            "frontend_detection_cam0_ended",
                            "frontend_matching_ended",
                            "frontend_detection_cami_ended",
                            "frontend_filter_ended",
                            "frontend_keypoints_pushed",
                            "backend_keypoints_received",
                            "backend_observations_processed",
                            "backend_cumulative_linearization_ended",
                            "backend_cumulative_solver_ended",
                            "backend_cumulative_backsubstitution_ended",
                            "backend_cumulative_error_computed",
                            "backend_optimization_ended",
                            "backend_marginalization_ended",
                            "backend_state_pushed",
                            "consumer_state_received"};

std::ostream& operator<<(std::ostream& os, const vit::TimeStats& s) {
  for (const int64_t& ts : s.timings) os << ts << (&ts != &s.timings.back() ? "," : "\n");
  return os;
}
}  // namespace

struct basalt_vio_ui : vis::VIOUIBase {
  VioDatasetPtr vio_dataset;
  int64_t start_t_ns = -1;
  size_t frame_count = 0;

  AutoScaleDataLog log_imu;
  AutoScaleDataLog log_vel, log_pos, log_ba, log_bg, log_point_count, log_ate, log_rte;
  std::unordered_map<KeypointId, size_t> keypoint_lifespans;
  shared_ptr<Plotter> plotter;

  pangolin::OpenGlRenderState camera;

  tbb::concurrent_bounded_queue<basalt::VioVisualizationData::Ptr> out_vis_queue;
  tbb::concurrent_bounded_queue<basalt::PoseVelBiasState<double>::Ptr> out_state_queue;

  std::vector<int64_t> vio_t_ns;
  Eigen::aligned_vector<Eigen::Vector3d> vio_t_w_i;
  Eigen::aligned_vector<Sophus::SE3d> vio_T_w_i;

  std::vector<int64_t> gt_t_ns;
  Eigen::aligned_vector<Eigen::Vector3d> gt_t_w_i;
  Eigen::aligned_vector<Sophus::SE3d> gt_T_w_i;
  std::vector<int64_t> keyframes_ts;

  std::string marg_data_path;
  size_t last_frame_processed = 0;

  tbb::concurrent_unordered_map<int64_t, int> timestamp_to_id;

  std::mutex m;
  std::condition_variable cvar;
  bool step_by_step = false;
  bool deterministic = false;
  bool save_times = false;
  size_t max_frames = 0;

  std::ofstream timing_csv{};

  std::atomic<bool> terminate = false;

  bool show_gui = true;
  std::string trajectory_fmt;
  std::string trajectory_name = "trajectory.csv";
  std::string result_path;
  bool trajectory_groundtruth = false;
  bool print_queue = false;
  std::chrono::high_resolution_clock::time_point time_start;
  bool aborted = false;
  bool initially_aligned = false;

  thread feed_images_thread;
  thread feed_imu_thread;
  thread vis_thread;
  thread ui_thread;
  thread state_consumer_thread;
  thread queues_printer_thread;

  Var<bool> trajectory_menu{"ui.Trajectory Menu", false, true};
  Var<string> trajectory_menu_title{"trajectory_menu.MENU", "Trajectory Menu", META_FLAG_READONLY};
  Var<bool> show_gt{"trajectory_menu.show_gt", true, true};
  Var<bool> show_full_gt{"trajectory_menu.show_full_gt", true, true};
  Button align_se3_btn{"trajectory_menu.align_se3", [this]() { alignButton(); }};
  Var<bool> euroc_fmt{"trajectory_menu.euroc_fmt", true, true};
  Var<bool> tum_rgbd_fmt{"trajectory_menu.tum_rgbd_fmt", false, true};
  Var<bool> kitti_fmt{"trajectory_menu.kitti_fmt", false, true};
  Var<bool> save_groundtruth{"trajectory_menu.save_groundtruth", false, true};
  Button save_traj_btn{"trajectory_menu.save_traj", [this]() { saveTrajectoryButton(); }};

  Button compute_frames_error_btn{"curves_menu.compute_frames_error", [this]() { compute_frames_error(); }};
  Var<int> rte_delta{"curves_menu.rte_delta", 6, 1, 30};
  Var<bool> show_frames_ate{"curves_menu.show_frames_ate", false, true};
  Var<bool> show_frames_rte{"curves_menu.show_frames_rte", false, true};

  Button next_step_btn{"ui.next_step", [this]() { next_step(); }};
  Button prev_step_btn{"ui.prev_step", [this]() { prev_step(); }};

  Var<bool> continue_btn{"ui.continue", true, true};
  Var<bool> continue_fast{"ui.continue_fast", true, true};

  shared_ptr<pangolin::VarValueGeneric> show_frame_bottom;

  struct OfflineVIOImageView : vis::VIOImageView {
    basalt_vio_ui& vio_ui;
    OfflineVIOImageView(basalt_vio_ui& ui) : VIOImageView(ui), vio_ui(ui) {}
    void Keyboard(View& view, unsigned char key, int x, int y, bool pressed) override {
      bool released = !pressed;
      if (key == ' ' && released) {
        vio_ui.continue_btn = !vio_ui.continue_btn;
        vio_ui.continue_btn.Meta().gui_changed = true;
      } else if (key == '.' && pressed) {
        vio_ui.next_step();
      } else if (key == ',' && pressed) {
        vio_ui.prev_step();
      } else if (key == '>' && pressed) {
        vio_ui.next_step(10);
      } else if (key == '<' && pressed) {
        vio_ui.prev_step(10);
      } else {
        vis::VIOImageView::Keyboard(view, key, x, y, pressed);
      }
    }
  };

  int start(int argc, char** argv) {
    bool print_queue = false;
    std::string cam_calib_path;
    std::string dataset_path;
    std::string dataset_type = "euroc";
    std::string config_path;
    int num_threads = 0;
    bool use_imu = true;
    bool use_double = false;

    CLI::App app{"Basalt CLI"};

    app.add_option("--show-gui", show_gui, "Show GUI");
    app.add_option("--cam-calib", cam_calib_path, "Ground-truth camera calibration used for simulation.")->required();
    app.add_option("--dataset-path", dataset_path, "Path to dataset.")->required();
    app.add_option("--dataset-type", dataset_type, "Dataset type <euroc, bag>.");
    app.add_option("--marg-data", marg_data_path, "Path to folder where marginalization data will be stored.");
    app.add_option("--print-queue", print_queue, "Print queue.");
    app.add_option("--config-path", config_path, "Path to config file.");
    app.add_option("--result-path", result_path, "Path to result file where the system will write RMSE ATE.");
    app.add_option("--num-threads", num_threads, "Number of threads.");
    app.add_option("--step-by-step", step_by_step, "Whether to wait for manual input before running the dataset");
    app.add_option("--save-trajectory", trajectory_fmt, "Save trajectory. Supported formats <tum, euroc, kitti>");
    app.add_option("--save-trajectory-fn", trajectory_name, "Name of the saved trajectory (default: trajectory.csv)");
    app.add_option("--save-groundtruth", trajectory_groundtruth, "In addition to trajectory, save also ground truth");
    app.add_option("--save-times", save_times, "Measure and save timing information.");
    app.add_option("--use-imu", use_imu, "Use IMU: visual-inertial vs visual-only pipeline");
    app.add_option("--use-double", use_double, "Use double not float.");
    app.add_option("--deterministic", deterministic, "Make the pipeline output reproducible (some performance impact)");
    app.add_option("--max-frames", max_frames, "Limit number of frames to process from dataset (0 means unlimited)");

    try {
      app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
      return app.exit(e);
    }

    // global thread limit is in effect until global_control object is destroyed
    std::unique_ptr<tbb::global_control> tbb_global_control;
    if (num_threads > 0) {
      tbb_global_control =
          std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism, num_threads);
    }

    if (!config_path.empty()) {
      config.load(config_path);

      if (config.vio_enforce_realtime) {
        config.vio_enforce_realtime = false;
        std::cout << "The option config.vio_enforce_realtime was enabled, "
                     "but it should only be used with the live executables (supply "
                     "images at a constant framerate). This executable runs on the "
                     "datasets and processes images as fast as it can, so the option "
                     "will be disabled. "
                  << std::endl;
      }
    }

    load_data(cam_calib_path);

    if (save_times) {
      string timing_fn = string("timing.") + trajectory_name;
      timing_csv = std::ofstream{timing_fn};
      timing_csv << "#";
      for (const auto& col : timing_titles) {
        string delimiter = &col != &timing_titles.back() ? "," : "\n";
        timing_csv << col << delimiter;
      }
    }

    {
      basalt::DatasetIoInterfacePtr dataset_io = basalt::DatasetIoFactory::getDatasetIo(dataset_type);

      dataset_io->read(dataset_path);

      vio_dataset = dataset_io->get_data();
      start_t_ns = vio_dataset->get_image_timestamps().front();
      frame_count = vio_dataset->get_image_timestamps().size();

      show_frame.Meta().range[1] = frame_count - 1;
      show_frame.Meta().gui_changed = true;

      opt_flow = basalt::OpticalFlowFactory::getOpticalFlow(config, calib);
      opt_flow->start();

      for (size_t i = 0; i < vio_dataset->get_gt_pose_data().size(); i++) {
        gt_t_ns.push_back(vio_dataset->get_gt_timestamps()[i]);
        gt_t_w_i.push_back(vio_dataset->get_gt_pose_data()[i].translation());
        gt_T_w_i.push_back(vio_dataset->get_gt_pose_data()[i]);
      }
    }

    {
      vio = basalt::VioEstimatorFactory::getVioEstimator(config, calib, basalt::constants::g, use_imu, use_double);
      vio->initialize(Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero());

      opt_flow->output_queue = &vio->vision_data_queue;
      opt_flow->show_gui = show_gui;
      if (show_gui) vio->out_vis_queue = &out_vis_queue;
      vio->out_state_queue = &out_state_queue;
      vio->opt_flow_depth_guess_queue = &opt_flow->input_depth_queue;
      vio->opt_flow_state_queue = &opt_flow->input_state_queue;
      vio->opt_flow_lm_bundle_queue = &opt_flow->input_lm_bundle_queue;
    }

    basalt::MargDataSaver::Ptr marg_data_saver;

    if (!marg_data_path.empty()) {
      marg_data_saver = make_shared<basalt::MargDataSaver>(marg_data_path);
      vio->out_marg_queue = &marg_data_saver->in_marg_queue;

      // Save gt.
      {
        std::string p = marg_data_path + "/gt.cereal";
        std::ofstream os(p, std::ios::binary);

        {
          cereal::BinaryOutputArchive archive(os);
          archive(gt_t_ns);
          archive(gt_t_w_i);
        }
        os.close();
      }
    }

    log_clear();

    if (step_by_step) {
      continue_btn = false;  // Disable automatically feeding next frame
      continue_fast = true;  // Move to the latest computed frame automatically
    }
    feed_images_thread = thread([this]() { feed_images(); });
    feed_imu_thread = thread([this]() { feed_imu(); });

    if (show_gui)
      vis_thread = thread([&]() {
        basalt::VioVisualizationData::Ptr data;

        while (true) {
          out_vis_queue.pop(data);
          if (!data) break;

          vis_map[data->t_ns] = data;
          log_vis_data(data);
          try_to_initially_align(data);
          // NOTE: keyframe_idx is only filled when the UI is enabled
          if (data->keyframed_idx.count(data->t_ns) > 0) keyframes_ts.push_back(data->t_ns);
        }

        std::cout << "Finished t3" << std::endl;
      });

    if (!deterministic) {
      state_consumer_thread = thread([&]() {
        while (pop_state()) continue;
      });
    }

    if (print_queue) {
      queues_printer_thread = thread([&]() {
        while (!terminate) {
          print_queue_fn();
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      });
    }

    time_start = std::chrono::high_resolution_clock::now();

    // record if we close the GUI before VIO is finished.
    aborted = false;

    if (show_gui) run_ui();

    return 0;
  }

  void log_clear() {
    log_vel.Clear();
    log_pos.Clear();
    log_ba.Clear();
    log_bg.Clear();
    log_point_count.Clear();
    log_ate.Clear();
    log_rte.Clear();
  }

  void log_vis_data(const VioVisualizationData::Ptr& data) {
    float t_s = (data->t_ns - start_t_ns) * 1e-9;

    std::vector<float> point_count;
    for (const auto& proj : *data->projections) point_count.push_back(proj.size());
    log_point_count.ScaledLog(t_s, point_count);

    for (const auto& cam_kps : data->opt_flow_res->keypoints)
      for (const auto& [kpid, _] : cam_kps) keypoint_lifespans[kpid]++;
  }

  bool try_to_initially_align(const VioVisualizationData::Ptr& data) {
    if (initially_aligned) return false;

    bool pose_moved_enough = !data->frames.empty() && data->frames.rbegin()->second.translation().norm() > 1.0f;
    if (pose_moved_enough) {
      alignButton();
      initially_aligned = true;
    }

    return false;
  }

  bool pop_state() {
    basalt::PoseVelBiasState<double>::Ptr data;
    out_state_queue.pop(data);

    if (data.get() == nullptr) return false;

    data->input_images->addTime("consumer_state_received");
    if (save_times) timing_csv << data->input_images->stats;  // Write CSV line

    int64_t t_ns = data->t_ns;

    Sophus::SE3d T_w_i = data->T_w_i;
    Eigen::Vector3d pos_w_i = T_w_i.translation();
    Eigen::Vector3d vel_w_i = data->vel_w_i;
    Eigen::Vector3d bg = data->bias_gyro;
    Eigen::Vector3d ba = data->bias_accel;

    vio_t_ns.emplace_back(data->t_ns);
    vio_t_w_i.emplace_back(pos_w_i);
    vio_T_w_i.emplace_back(T_w_i);

    if (show_gui) {
      double t_s = (t_ns - start_t_ns) * 1e-9;
      log_vel.ScaledLog(t_s, {vel_w_i[0], vel_w_i[1], vel_w_i[2]});
      log_pos.ScaledLog(t_s, {pos_w_i[0], pos_w_i[1], pos_w_i[2]});
      log_ba.ScaledLog(t_s, {ba[0], ba[1], ba[2]});
      log_bg.ScaledLog(t_s, {bg[0], bg[1], bg[2]});
    }

    return true;
  }

  void print_queue_fn() {
    std::cout << "opt_flow->input_img_queue " << opt_flow->input_img_queue.size() << " opt_flow->output_queue "
              << opt_flow->output_queue->size() << " out_state_queue " << out_state_queue.size() << " imu_data_queue "
              << vio->imu_data_queue.size() << std::endl;
  }

  void run_ui() {
    if (show_gui) {
      pangolin::CreateWindowAndBind("Main", 1800, 1000, vis::default_win_params);

      glEnable(GL_DEPTH_TEST);

      View& main_display = pangolin::CreateDisplay();
      main_display.SetBounds(UI_BOTTOM, 1.0, UI_WIDTH, 1.0);

      img_view_display = &pangolin::CreateDisplay();
      img_view_display->SetBounds(0.4, 1.0, 0.0, 0.4);
      img_view_display->SetLayout(pangolin::LayoutEqual);

      plotter = make_shared<Plotter>(&log_imu, 0.0, 100, -10.0, 10.0, 0.01, 0.01);
      plotter->SetBackgroundColour(vis::C_BLUEGREY_DARK());
      plotter->SetAxisColour(vis::C_BLUEGREY_LIGHT());
      plotter->SetTickColour(vis::C_BLUEGREY());
      plot_display = &pangolin::CreateDisplay();
      plot_display->SetBounds(UI_BOTTOM, 0.4, UI_WIDTH, 1.0);
      plot_display->AddDisplay(*plotter);

      blocks_view = make_shared<pangolin::ImageView>();
      blocks_view->UseNN() = true;  // Disable antialiasing, can be toggled with N key
      blocks_view->extern_draw_function = [this](View& /*v*/) { draw_blocks_overlay(); };
      const int DEFAULT_W = 480;
      blocks_display = &pangolin::CreateDisplay();
      blocks_display->SetBounds(0.0, 0.6, UI_WIDTH, pangolin::Attach::Pix(UI_WIDTH_PIX + DEFAULT_W));
      blocks_display->AddDisplay(*blocks_view);
      blocks_display->Show(show_blocks);

      pangolin::CreatePanel("ui").SetBounds(UI_BOTTOM, 1.0, 0.0, UI_WIDTH);
      pangolin::CreatePanel("bottom_panel").SetBounds(0.0, UI_BOTTOM, 0.0, 1.0);
      int& show_frame_ref = const_cast<int&>(show_frame.Get());  // HACK: pangolin makes this difficult otherwise
      pangolin::AttachVar("bottom_panel.show_frame", show_frame_ref, 0, vio_dataset->get_image_timestamps().size() - 1);
      show_frame_bottom = pangolin::VarState::I().GetByName("bottom_panel.show_frame");

      menus.push_back(&trajectory_menu);
      menus_str.emplace_back("trajectory_menu");
      for (size_t i = 0; i < menus.size(); i++) {
        pangolin::CreatePanel(menus_str[i]).SetBounds(UI_BOTTOM, 0.4, 0.0, UI_WIDTH);
        pangolin::Display(menus_str[i]).Show(*menus[i]);
      }

      while (img_view.size() < calib.intrinsics.size()) {
        auto iv = make_shared<OfflineVIOImageView>(*this);
        iv->UseNN() = true;  // Disable antialiasing, can be toggled with N key

        size_t idx = img_view.size();
        img_view.push_back(iv);

        img_view_display->AddDisplay(*iv);
        iv->extern_draw_function = [idx, this](View& v) {
          return draw_image_overlay(dynamic_cast<pangolin::ImageView&>(v), idx);
        };
      }

      Eigen::Vector3d cam_p(-0.5, -3, -5);
      cam_p = vio->getT_w_i_init().so3() * calib.T_i_c[0].so3() * cam_p;

      camera = pangolin::OpenGlRenderState(
          pangolin::ProjectionMatrix(640, 480, 400, 400, 320, 240, 0.001, 10000),
          pangolin::ModelViewLookAt(cam_p[0], cam_p[1], cam_p[2], 0, 0, 0, pangolin::AxisZ));

      View& display3D = pangolin::CreateDisplay();
      display3D.SetAspect(-640 / 480.0);
      display3D.SetBounds(0.4, 1.0, 0.4, 1.0);
      display3D.SetHandler(new pangolin::Handler3D(camera));

      display3D.extern_draw_function = [this](View& v) { draw_scene(v); };

      main_display.AddDisplay(*img_view_display);
      main_display.AddDisplay(display3D);

      while (!pangolin::ShouldQuit()) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        size_t frame_id = show_frame;
        int64_t t_ns = vio_dataset->get_image_timestamps()[frame_id];
        auto it = vis_map.find(t_ns);
        if (follow) {
          if (it != vis_map.end()) {
            Sophus::SE3d T_w_i;
            if (!it->second->states.empty()) {
              T_w_i = it->second->states.rbegin()->second;
            } else if (!it->second->frames.empty()) {
              T_w_i = it->second->frames.rbegin()->second;
            }
            T_w_i.so3() = Sophus::SO3d();

            camera.Follow(T_w_i.matrix());
          }
        }

        display3D.Activate(camera);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);

        img_view_display->Activate();
        if (fixed_depth.GuiChanged() && vio->opt_flow_depth_guess_queue != nullptr) {
          vio->opt_flow_depth_guess_queue->push(fixed_depth);
          depth_guess = fixed_depth;
        } else if (it != vis_map.end() && it->second->opt_flow_res && it->second->opt_flow_res->input_images) {
          depth_guess = it->second->opt_flow_res->input_images->depth_guess;
        }

        if (show_frame_bottom->Meta().gui_changed) {
          show_frame_bottom->Meta().gui_changed = false;
          show_frame.Meta().gui_changed = true;
        };

        if (show_frame.GuiChanged()) {
          auto frame_id = static_cast<size_t>(show_frame);
          int64_t timestamp = vio_dataset->get_image_timestamps()[frame_id];
          std::vector<basalt::ImageData> img_vec = vio_dataset->get_image_data(timestamp);
          for (size_t cam_id = 0; cam_id < calib.intrinsics.size(); cam_id++) {
            pangolin::GlPixFormat fmt;
            fmt.glformat = GL_LUMINANCE;
            fmt.gltype = GL_UNSIGNED_SHORT;
            fmt.scalable_internal_format = GL_LUMINANCE16;

            if (cam_id < img_vec.size() && img_vec[cam_id].img && img_vec[cam_id].img)
              img_view[cam_id]->SetImage(img_vec[cam_id].img->ptr, img_vec[cam_id].img->w, img_vec[cam_id].img->h,
                                         img_vec[cam_id].img->pitch, fmt);
          }
          if (follow_highlight) do_follow_highlight(true, false);

          if (show_blocks) do_show_blocks();

          draw_plots();
        }

        static const std::array curves_ticks = {&show_est_vel,     &show_est_pos,    &show_est_ba,    &show_est_bg,
                                                &show_point_count, &show_frames_ate, &show_frames_rte};
        static const std::array curves_logs = {&log_vel,         &log_pos, &log_ba, &log_bg,
                                               &log_point_count, &log_ate, &log_rte};

        for (size_t i = 0; i < curves_ticks.size(); i++) {
          if (!curves_ticks[i]->GuiChanged()) continue;
          draw_plots();
          if (*curves_ticks[i]) curves_logs[i]->FitPlotter(*plotter);
          break;
        }

        if (highlight_landmarks.GuiChanged() || filter_highlights.GuiChanged() || show_highlights.GuiChanged() ||
            show_ids.GuiChanged()) {
          highlights = vis::parse_selection(highlight_landmarks);
          filter_highlights = filter_highlights && !highlights.empty();
          for (const auto& [ts, vis] : vis_map) vis->invalidate_mat_imgs();
          if (show_blocks) do_show_blocks();
        }

        if (mat_to_show.GuiChanged()) {
          mat_name = std::string(magic_enum::enum_name((UIMAT)mat_to_show.Get()));
          if (show_blocks) do_show_blocks();
        }

        if (follow_highlight.GuiChanged()) {
          follow_highlight = follow_highlight && !highlights.empty();
          do_follow_highlight(follow_highlight, true);
        }

        if (euroc_fmt.GuiChanged()) {
          euroc_fmt = true;
          tum_rgbd_fmt = false;
          kitti_fmt = false;
        }

        if (tum_rgbd_fmt.GuiChanged()) {
          tum_rgbd_fmt = true;
          euroc_fmt = false;
          kitti_fmt = false;
        }

        if (kitti_fmt.GuiChanged()) {
          kitti_fmt = true;
          euroc_fmt = false;
          tum_rgbd_fmt = false;
        }

        Var<bool>* selected_menu = nullptr;
        for (Var<bool>* menu : menus) {
          if (menu->GuiChanged()) {
            selected_menu = menu;
            break;
          }
        }
        if (selected_menu != nullptr) {
          for (size_t i = 0; i < menus.size(); i++) {
            *menus[i] = menus[i] != selected_menu ? false : *selected_menu;
            pangolin::Display(menus_str[i]).Show(*menus[i]);
          }
        }

        //      if (record) {
        //        main_display.RecordOnRender(
        //            "ffmpeg:[fps=50,bps=80000000,unique_filename]///tmp/"
        //            "vio_screencap.avi");
        //        record = false;
        //      }

        pangolin::FinishFrame();

        if (continue_btn) {
          if (!next_step()) std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        if (continue_fast) {
          int64_t t_ns = vio->last_processed_t_ns;
          if (timestamp_to_id.count(t_ns)) {
            show_frame = timestamp_to_id[t_ns];
            show_frame.Meta().gui_changed = true;
          }

          if (vio->finished) {
            continue_btn = false;
            continue_fast = false;
          }
        }
      }

      // If GUI closed but VIO not yet finished --> abort input queues, which in
      // turn aborts processing
      if (!vio->finished) {
        std::cout << "GUI closed but odometry still running --> aborting.\n";
        print_queue_fn();  // print queue size at time of aborting
        terminate = true;
        aborted = true;
      }
    }
  }

  void stop() {
    if (!vio) return;

    // wait first for vio to complete processing
    vio->maybe_join();

    // input threads will abort when vio is finished, but might be stuck in full
    // push to full queue, so drain queue now
    vio->drain_input_queues();
    opt_flow->drain_input_queues();

    // join input threads
    feed_images_thread.join();
    feed_imu_thread.join();

    // std::cout << "Data input finished, terminate auxiliary threads.";
    terminate = true;

    // join other threads
    if (show_gui) vis_thread.join();
    if (!deterministic) state_consumer_thread.join();
    if (print_queue) queues_printer_thread.join();

    // after joining all threads, print final queue sizes.
    if (print_queue) {
      std::cout << "Final queue sizes:" << std::endl;
      print_queue_fn();
    }

    auto time_end = std::chrono::high_resolution_clock::now();
    const double duration_total = std::chrono::duration<double>(time_end - time_start).count();

    // TODO: remove this unconditional call (here for debugging);
    const double ate_rmse = basalt::alignSVD(vio_t_ns, vio_t_w_i, gt_t_ns, gt_t_w_i);
    vio->debug_finalize();
    std::cout << "Total runtime: {:.3f}s\n"_format(duration_total);

    {
      basalt::ExecutionStats stats;
      stats.add("exec_time_s", duration_total);
      stats.add("ate_rmse", ate_rmse);
      stats.add("ate_num_kfs", vio_t_w_i.size());
      stats.add("num_frames", frame_count);

      {
        basalt::MemoryInfo mi;
        if (get_memory_info(mi)) {
          stats.add("resident_memory_peak", mi.resident_memory_peak);
        }
      }

      stats.save_json("stats_vio.json");
    }

    if (!aborted && !trajectory_fmt.empty()) {
      std::cout << "Saving trajectory..." << std::endl;

      if (trajectory_fmt == "kitti") {
        kitti_fmt = true;
        euroc_fmt = false;
        tum_rgbd_fmt = false;
      }
      if (trajectory_fmt == "euroc") {
        euroc_fmt = true;
        kitti_fmt = false;
        tum_rgbd_fmt = false;
      }
      if (trajectory_fmt == "tum") {
        tum_rgbd_fmt = true;
        euroc_fmt = false;
        kitti_fmt = false;
      }

      save_groundtruth = trajectory_groundtruth;

      saveTrajectoryButton();
    }

    if (!aborted && !result_path.empty()) {
      double error = basalt::alignSVD(vio_t_ns, vio_t_w_i, gt_t_ns, gt_t_w_i);

      auto exec_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time_end - time_start);

      std::ofstream os(result_path);
      {
        cereal::JSONOutputArchive ar(os);
        ar(cereal::make_nvp("rms_ate", error));
        ar(cereal::make_nvp("num_frames", frame_count));
        ar(cereal::make_nvp("exec_time_ns", exec_time_ns.count()));
      }
      os.close();
    }

    if (save_times) timing_csv.close();

    // Report average track length
    if (show_gui) {
      size_t total = 0;
      for (const auto& [kpid, lifespan] : keypoint_lifespans) total += lifespan;

      double avg = double(total) / double(keypoint_lifespans.size());
      std::cout << "Average track length: " << avg << " frames\n";
    }
  }

  VioVisualizationData::Ptr get_curr_vis_data() override {
    int64_t curr_ts = vio_dataset->get_image_timestamps().at(show_frame);
    auto it = vis_map.find(curr_ts);
    if (it == vis_map.end()) return nullptr;
    VioVisualizationData::Ptr curr_vis_data = it->second;
    return curr_vis_data;
  }

  OpticalFlowInput::Ptr load_frameset(size_t idx) {
    const int num_cams = calib.intrinsics.size();

    int64_t t_ns = vio_dataset->get_image_timestamps()[idx];
    auto img = make_shared<OpticalFlowInput>(num_cams);

    if (save_times) {
      img->stats.ts = t_ns;
      img->stats.enabled_exts.has_pose_timing = true;
      img->stats.timings.reserve(timing_titles.size());  // This enables timing measurements in the pipeline
      img->stats.timing_titles = timing_titles.data();
    }
    img->addTime("frames_original_timestamp", t_ns);
    img->addTime("frames_read_started");

    img->t_ns = t_ns;
    img->img_data = vio_dataset->get_image_data(img->t_ns);

    if (img->img_data.size() != size_t(num_cams)) {
      std::cout << "Skipping incomplete frameset for timestamp " << img->t_ns << std::endl;
      return nullptr;
    }
    img->addTime("frames_read");

    timestamp_to_id[img->t_ns] = idx;

    return img;
  }

  // Feed functions
  void feed_images() {
    std::cout << "Started input_data thread " << std::endl;

    BASALT_ASSERT(frame_count > 0);

    OpticalFlowInput::Ptr img = nullptr;

    size_t i = 0;  // Read the first valid frameset
    while (img == nullptr && i < frame_count) {
      img = load_frameset(i);
      i++;
    }

    for (; i < frame_count; i++) {
      if (vio->finished || terminate || (max_frames > 0 && i >= max_frames)) break;

      if (step_by_step) {
        std::unique_lock<std::mutex> lk(m);
        cvar.wait(lk);
      }

      img->addTime("frames_pushed");
      opt_flow->input_img_queue.push(img);  // Push to frontend

      if (i % 64 == 0) {
        float completion = 100.0 * i / frame_count;
        std::cout << "[{:.2f}%] Input image {}/{}\t\r"_format(completion, i, frame_count) << std::flush;
      }

      img = load_frameset(i);  // Load next frameset while the previous is being processed

      if (deterministic) pop_state();  // Wait for the state to be produced
    }

    if (img) {  // Push and wait for the last frameset if any
      img->addTime("frames_pushed");
      opt_flow->input_img_queue.push(img);
      if (deterministic) pop_state();
    }

    // Indicate the end of the sequence
    opt_flow->input_img_queue.push(nullptr);

    std::cout << "Finished input_data thread " << std::endl;
  }

  void feed_imu() {
    for (size_t i = 0; i < vio_dataset->get_gyro_data().size(); i++) {
      if (vio->finished || terminate) {
        break;
      }

      basalt::ImuData<double>::Ptr data(new basalt::ImuData<double>);
      data->t_ns = vio_dataset->get_gyro_data()[i].timestamp_ns;

      data->accel = vio_dataset->get_accel_data()[i].data;
      data->gyro = vio_dataset->get_gyro_data()[i].data;

      vio->imu_data_queue.push(data);
      opt_flow->input_imu_queue.push(data);
    }
    vio->imu_data_queue.push(nullptr);
    opt_flow->input_imu_queue.push(nullptr);
  }

  void draw_image_overlay(pangolin::ImageView& v, size_t cam_id) {
    UNUSED(v);
    VioVisualizationData::Ptr curr_vis_data = get_curr_vis_data();
    if (curr_vis_data == nullptr) return;

    if (show_obs) do_show_obs(cam_id);
    if (show_flow) do_show_flow(cam_id);
    if (show_highlights) do_show_highlights(cam_id);
    if (show_tracking_guess) do_show_tracking_guess_vio(cam_id, show_frame, vio_dataset, vis_map);
    if (show_matching_guess) do_show_matching_guesses(cam_id);
    if (show_recall_guess) do_show_recall_guesses(cam_id);
    if (show_masks) do_show_masks(cam_id);
    if (show_cam0_proj) do_show_cam0_proj(cam_id, depth_guess);
    if (show_grid) do_show_grid();
    if (show_safe_radius) do_show_safe_radius();
    if (show_keyframe) do_show_keyframe();
  }

  void draw_scene(View& view) {
    UNUSED(view);
    view.Activate(camera);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);

    glPointSize(3);
    glColor3f(1.0, 0.0, 0.0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glColor3ubv(cam_color);
    if (!vio_t_w_i.empty()) {
      size_t end = std::min(vio_t_w_i.size(), size_t(show_frame + 1));
      Eigen::aligned_vector<Eigen::Vector3d> sub_gt(vio_t_w_i.begin(), vio_t_w_i.begin() + end);
      pangolin::glDrawLineStrip(sub_gt);
    }

    glColor3ubv(gt_color);
    if (show_gt && show_full_gt)
      pangolin::glDrawLineStrip(gt_t_w_i);
    else if (show_gt) {
      int64_t curr_ts = vio_dataset->get_image_timestamps().at(show_frame);
      auto it = std::lower_bound(gt_t_ns.begin(), gt_t_ns.end(), curr_ts);
      size_t end = std::distance(gt_t_ns.begin(), it);
      Eigen::aligned_vector<Eigen::Vector3d> sub_gt(gt_t_w_i.begin(), gt_t_w_i.begin() + end);
      pangolin::glDrawLineStrip(sub_gt);
    }

    VioVisualizationData::Ptr curr_vis_data = get_curr_vis_data();
    if (curr_vis_data == nullptr) return;

    const uint8_t cs[4][3]{{244, 67, 54}, {76, 175, 80}, {33, 150, 243}, {255, 152, 0}};  // r, g, b, orange
    auto cam_color = [&cs](size_t i) -> const uint8_t* { return cs[i % 4]; };
    if (!curr_vis_data->states.empty() || !curr_vis_data->frames.empty()) {
      const auto& [ts, p] = *curr_vis_data->states.rbegin();
      pangolin::glDrawAxis(p.matrix(), 0.05);
      for (size_t i = 0; i < calib.T_i_c.size(); i++) do_render_camera(p * calib.T_i_c[i], i, ts, cam_color(i));
    } else if (!curr_vis_data->frames.empty()) {
      const auto& [ts, p] = *curr_vis_data->frames.rbegin();
      pangolin::glDrawAxis(p.matrix(), 0.05);
      for (size_t i = 0; i < calib.T_i_c.size(); i++) do_render_camera(p * calib.T_i_c[i], i, ts, cam_color(i));
    }

    for (const auto& [ts, p] : curr_vis_data->states)
      for (size_t i = 0; i < calib.T_i_c.size(); i++) do_render_camera(p * calib.T_i_c[i], i, ts, state_color);

    for (const auto& [ts, p] : curr_vis_data->frames)
      for (size_t i = 0; i < calib.T_i_c.size(); i++) do_render_camera(p * calib.T_i_c[i], i, ts, pose_color);

    for (const auto& [ts, p] : curr_vis_data->ltframes)
      for (size_t i = 0; i < calib.T_i_c.size(); i++) do_render_camera(p * calib.T_i_c[i], i, ts, vis::BLUE);

    glColor3ubv(pose_color);
    if (!filter_highlights) pangolin::glDrawPoints(curr_vis_data->points);

    Eigen::aligned_vector<Eigen::Vector3d> highlighted_points;
    if (show_highlights || filter_highlights) {
      for (size_t i = 0; i < curr_vis_data->point_ids.size(); i++) {
        Vector3d pos = curr_vis_data->points.at(i);
        int id = curr_vis_data->point_ids.at(i);
        if (is_highlighted(id)) highlighted_points.push_back(pos);
      }
    }

    if (filter_highlights) pangolin::glDrawPoints(highlighted_points);

    if (show_highlights) {
      glColor3ubv(vis::GREEN);
      glPointSize(10);
      pangolin::glDrawPoints(highlighted_points);
    }

    glColor3ubv(pose_color);
    if (show_ids) {
      for (size_t i = 0; i < curr_vis_data->points.size(); i++) {
        Vector3d pos = curr_vis_data->points.at(i);
        int id = curr_vis_data->point_ids.at(i);

        bool highlighted = is_highlighted(id);
        if (filter_highlights && !highlighted) continue;

        if (show_highlights && highlighted) glColor3ubv(vis::GREEN);
        FONT.Text("%d", id).Draw(pos.x(), pos.y(), pos.z());
        if (show_highlights && highlighted) glColor3ubv(pose_color);
      }
    }

    pangolin::glDrawAxis(Sophus::SE3d().matrix(), 1.0);
  }

  void load_data(const std::string& calib_path) {
    std::ifstream os(calib_path, std::ios::binary);

    if (os.is_open()) {
      cereal::JSONInputArchive archive(os);
      archive(calib);
      std::cout << "Loaded camera with " << calib.intrinsics.size() << " cameras" << std::endl;

    } else {
      std::cerr << "could not load camera calibration " << calib_path << std::endl;
      std::abort();
    }

    double baseline = (calib.T_i_c[1].translation() - calib.T_i_c[0].translation()).norm();
    if (baseline < config.vio_min_triangulation_dist)
      std::cout << "Warning: baseline (" << baseline << ") is smaller than vio_min_triangulation_dist ("
                << config.vio_min_triangulation_dist << "). Update the config file." << std::endl;
  }

  bool next_step(int steps = 1) {
    if (show_frame < int(frame_count) - steps) {
      show_frame = show_frame + steps;
      show_frame.Meta().gui_changed = true;
      cvar.notify_one();
      return true;
    } else {
      return false;
    }
  }

  bool prev_step(int steps = 1) {
    if (show_frame >= steps) {
      show_frame = show_frame - steps;
      show_frame.Meta().gui_changed = true;
      return true;
    } else {
      return false;
    }
  }

  void draw_plots() {
    plotter->ClearSeries();
    plotter->ClearMarkers();

    if (show_est_pos) {
      plotter->AddSeries("$0", "$1", pangolin::DrawingModeLine, vis::C_RED(), "position x", &log_pos);
      plotter->AddSeries("$0", "$2", pangolin::DrawingModeLine, vis::C_GREEN(), "position y", &log_pos);
      plotter->AddSeries("$0", "$3", pangolin::DrawingModeLine, vis::C_BLUE(), "position z", &log_pos);
    }

    if (show_est_vel) {
      plotter->AddSeries("$0", "$1", pangolin::DrawingModeLine, vis::C_RED_DARK(), "velocity x", &log_vel);
      plotter->AddSeries("$0", "$2", pangolin::DrawingModeLine, vis::C_GREEN_DARK(), "velocity y", &log_vel);
      plotter->AddSeries("$0", "$3", pangolin::DrawingModeLine, vis::C_BLUE_DARK(), "velocity z", &log_vel);
    }

    if (show_est_bg) {
      plotter->AddSeries("$0", "$1", pangolin::DrawingModeLine, vis::C_RED(), "gyro bias x", &log_bg);
      plotter->AddSeries("$0", "$2", pangolin::DrawingModeLine, vis::C_GREEN(), "gyro bias y", &log_bg);
      plotter->AddSeries("$0", "$3", pangolin::DrawingModeLine, vis::C_BLUE(), "gyro bias z", &log_bg);
    }

    if (show_est_ba) {
      plotter->AddSeries("$0", "$1", pangolin::DrawingModeLine, vis::C_RED_DARK(), "accel bias x", &log_ba);
      plotter->AddSeries("$0", "$2", pangolin::DrawingModeLine, vis::C_GREEN_DARK(), "accel bias y", &log_ba);
      plotter->AddSeries("$0", "$3", pangolin::DrawingModeLine, vis::C_BLUE_DARK(), "accel bias z", &log_ba);
    }

    const size_t N = calib.intrinsics.size();
    if (show_point_count) {
      std::array<pangolin::Colour, 3> colors = {vis::C_GREEN(), vis::C_GREEN_DARK(), vis::C_GREEN_LIGHT()};
      for (size_t i = 0; i < N; i++)
        plotter->AddSeries("$0", "$" + std::to_string(i + 1), pangolin::DrawingModeLine, colors[i % colors.size()],
                           "point count cam" + std::to_string(i), &log_point_count);
    }

    if (show_frames_ate) {
      plotter->AddSeries("$0", "$1", pangolin::DrawingModeLine, vis::C_PINK_DARK(), "ATE [10µm]", &log_ate);
      plotter->AddSeries("$0", "$2", pangolin::DrawingModeLine, vis::C_PINK(), "ATE diffs [µm]", &log_ate);
    }

    if (show_frames_rte) {
      plotter->AddSeries("$0", "$1", pangolin::DrawingModeLine, vis::C_AMBER_DARK(), "RTE [10µm]", &log_rte);
      plotter->AddSeries("$0", "$2", pangolin::DrawingModeLine, vis::C_AMBER(), "RTE diffs [µm]", &log_rte);
      plotter->AddSeries("$0", "$3", pangolin::DrawingModeLine, vis::C_AMBER_LIGHT(), "RTE residuals [µm]", &log_rte);
    }

    double t = (vio_dataset->get_image_timestamps()[show_frame] - start_t_ns) * 1e-9;
    plotter->AddMarker(pangolin::Marker::Vertical, t, pangolin::Marker::Equal, pangolin::Colour::White());
  }

  void alignButton() { basalt::alignSVD(vio_t_ns, vio_t_w_i, gt_t_ns, gt_t_w_i); }

  void compute_frames_error() {
    Eigen::Matrix<int64_t, Eigen::Dynamic, 1> est_ts{};
    Eigen::Matrix<float, 3, Eigen::Dynamic> est_xyz{};
    Eigen::Matrix<float, 3, Eigen::Dynamic> ref_xyz{};
    Eigen::Matrix<float, 4, Eigen::Dynamic> est_quat{};
    Eigen::Matrix<float, 4, Eigen::Dynamic> ref_quat{};

    // Compute estimate-gt associations
    int num_assocs = associate(vio_t_ns, vio_T_w_i, gt_t_ns, gt_T_w_i, est_ts, est_xyz, ref_xyz, est_quat, ref_quat);

    // Compute ATE

    log_ate.Clear();
    constexpr int SAMPLE_POINTS = 10000;  // Maximum points to plot
    if (num_assocs > SAMPLE_POINTS)
      std::cout << "To many frames (" << num_assocs << "), sampling only " << SAMPLE_POINTS << std::endl;

    int sample_points = std::min(SAMPLE_POINTS, num_assocs);
    int step = num_assocs / sample_points;

    float final_ate = 0;
    Eigen::Matrix4f T_ref_est{};
    log_ate.ScaledLog(0, {0, 0});
    for (int i = 1; i <= num_assocs; i += step) {
      T_ref_est = get_alignment(est_xyz, ref_xyz, 0, i);
      float ate = compute_ate(est_xyz, ref_xyz, T_ref_est, 0, i);
      log_ate.ScaledLog(float((est_ts(i - 1) - start_t_ns) * 1e-9), {ate * 1e5f, (ate - final_ate) * 1e6f});
      final_ate = ate;
    }

    // Compute RTE

    log_rte.Clear();
    Eigen::Matrix<int64_t, Eigen::Dynamic, 1> rte_ts{};
    Eigen::Matrix<float, Eigen::Dynamic, 1> rte_residuals{};
    compute_rte(est_ts, est_xyz, est_quat, ref_xyz, ref_quat, rte_ts, rte_residuals, 0, num_assocs, rte_delta);

    float sqres_sum = 0;
    float final_rte = 0;
    log_rte.ScaledLog(0, {0, 0, 0});
    for (int i = 1; i < rte_residuals.rows(); i++) {
      float residual = rte_residuals(i);
      sqres_sum += residual * residual;
      float rte = std::sqrt(sqres_sum / i);
      log_rte.ScaledLog(float((rte_ts(i) - start_t_ns) * 1e-9),
                        {rte * 1e5f, (rte - final_rte) * 1e6f, residual * 1e6f});
      final_rte = rte;
    }

    std::cout << "[Errors]\n";
    std::cout << "ATE " << final_ate << std::endl;
    std::cout << "RTE " << final_rte << std::endl;
    std::cout << "T_align\n" << T_ref_est.matrix() << std::endl;
    std::cout << "number of associations " << num_assocs << std::endl;

    show_frames_ate = true;
    show_frames_ate.Meta().gui_changed = true;
    show_frames_rte = true;
    show_frames_rte.Meta().gui_changed = true;
  }

  void saveTrajectoryButton() {
    if (tum_rgbd_fmt) {
      {
        std::ofstream os(trajectory_name);

        os << "# timestamp tx ty tz qx qy qz qw" << std::endl;

        for (size_t i = 0; i < vio_t_ns.size(); i++) {
          const Sophus::SE3d& pose = vio_T_w_i[i];
          os << std::scientific << std::setprecision(18) << vio_t_ns[i] * 1e-9 << " " << pose.translation().x() << " "
             << pose.translation().y() << " " << pose.translation().z() << " " << pose.unit_quaternion().x() << " "
             << pose.unit_quaternion().y() << " " << pose.unit_quaternion().z() << " " << pose.unit_quaternion().w()
             << std::endl;
        }

        os.close();
      }

      if (save_groundtruth) {
        std::ofstream os("groundtruth.txt");

        os << "# timestamp tx ty tz qx qy qz qw" << std::endl;

        for (size_t i = 0; i < gt_t_ns.size(); i++) {
          const Eigen::Vector3d& pos = gt_t_w_i[i];
          os << std::scientific << std::setprecision(18) << gt_t_ns[i] * 1e-9 << " " << pos.x() << " " << pos.y() << " "
             << pos.z() << " 0 0 0 1" << std::endl;
        }

        os.close();
      }

      std::cout << "Saved trajectory in TUM RGB-D Dataset format in " << trajectory_name << std::endl;
    } else if (euroc_fmt) {
      std::ofstream os(trajectory_name);

      os << "#timestamp [ns],p_RS_R_x [m],p_RS_R_y [m],p_RS_R_z [m],q_RS_w "
            "[],q_RS_x [],q_RS_y [],q_RS_z []"
         << std::endl;

      for (size_t i = 0; i < vio_t_ns.size(); i++) {
        const Sophus::SE3d& pose = vio_T_w_i[i];
        os << std::fixed << std::setprecision(10) << vio_t_ns[i] << "," << pose.translation().x() << ","
           << pose.translation().y() << "," << pose.translation().z() << "," << pose.unit_quaternion().w() << ","
           << pose.unit_quaternion().x() << "," << pose.unit_quaternion().y() << "," << pose.unit_quaternion().z()
           << std::endl;
      }

      std::cout << "Saved trajectory in Euroc Dataset format in " << trajectory_name << std::endl;
    } else {
      std::ofstream os(trajectory_name);

      for (size_t i = 0; i < vio_t_ns.size(); i++) {
        Eigen::Matrix<double, 3, 4> mat = vio_T_w_i[i].matrix3x4();
        os << std::scientific << std::setprecision(12) << mat.row(0) << " " << mat.row(1) << " " << mat.row(2) << " "
           << std::endl;
      }

      os.close();

      std::cout << "Saved trajectory in KITTI Dataset format in " << trajectory_name << std::endl;
    }
  }
};

int main(int argc, char** argv) {
  basalt_vio_ui u{};

  int ret = u.start(argc, argv);
  if (ret != 0) return ret;

  u.stop();

  return 0;
}
