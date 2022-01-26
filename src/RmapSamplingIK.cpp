/* Author: Masaki Murooka */

#include <sch/S_Polyhedron/S_Polyhedron.h>

#include <optmotiongen_msgs/RobotStateArray.h>

#include <optmotiongen/Utils/RosUtils.h>

#include <differentiable_rmap/RmapSamplingIK.h>

using namespace DiffRmap;


namespace
{
/** \brief Get selection indices of task value depending on sampling space. */
std::vector<size_t> getSelectIdxs(SamplingSpace sampling_space)
{
  switch (sampling_space) {
    case SamplingSpace::R2:
      return std::vector<size_t>{3, 4};
    case SamplingSpace::SO2:
      return std::vector<size_t>{2};
    case SamplingSpace::SE2:
      return std::vector<size_t>{2, 3, 4};
    case SamplingSpace::R3:
      return std::vector<size_t>{3, 4, 5};
    case SamplingSpace::SO3:
      return std::vector<size_t>{0, 1, 2};
    case SamplingSpace::SE3:
      return std::vector<size_t>{0, 1, 2, 3, 4, 5};
    default:
      mc_rtc::log::error_and_throw<std::runtime_error>(
          "[getSelectIdxs] SamplingSpace {} is not supported.", std::to_string(sampling_space));
  }
}
}

template <SamplingSpace SamplingSpaceType>
RmapSamplingIK<SamplingSpaceType>::RmapSamplingIK(
    const std::shared_ptr<OmgCore::Robot>& rb):
    RmapSampling<SamplingSpaceType>(rb)
{
  collision_marker_pub_ =
      nh_.template advertise<visualization_msgs::MarkerArray>("collision_marker", 1, true);
}

template <SamplingSpace SamplingSpaceType>
RmapSamplingIK<SamplingSpaceType>::RmapSamplingIK(
    const std::shared_ptr<OmgCore::Robot>& rb,
    const std::string& body_name,
    const std::vector<std::string>& joint_name_list):
    RmapSampling<SamplingSpaceType>(rb, body_name, joint_name_list)
{
  collision_marker_pub_ =
      nh_.template advertise<visualization_msgs::MarkerArray>("collision_marker", 1, true);
}

template <SamplingSpace SamplingSpaceType>
void RmapSamplingIK<SamplingSpaceType>::configure(const mc_rtc::Configuration& mc_rtc_config)
{
  RmapSampling<SamplingSpaceType>::configure(mc_rtc_config);
  config_.load(mc_rtc_config);
}

template <SamplingSpace SamplingSpaceType>
void RmapSamplingIK<SamplingSpaceType>::setupSampling()
{
  // Set robot root pose
  rb_arr_[0]->rootPose(config_.root_pose);

  // Setup task
  SamplingSpace ik_constraint_space = SamplingSpaceType;
  if (!config_.ik_constraint_space.empty()) {
    ik_constraint_space = strToSamplingSpace(config_.ik_constraint_space);
  }
  body_task_ = std::make_shared<OmgCore::BodyPoseTask>(
      std::make_shared<OmgCore::BodyFunc>(
          rb_arr_,
          0,
          body_name_,
          config_.body_pose_offset),
      sva::PTransformd::Identity(),
      "BodyPoseTask",
      getSelectIdxs(ik_constraint_space));

  setupCollisionTask();

  // Setup problem
  taskset_.addTask(body_task_);
  for (const auto& collision_task : collision_task_list_) {
    taskset_.addTask(collision_task);
  }

  problem_ = std::make_shared<OmgCore::IterativeQpProblem>(rb_arr_);
  // JRLQP is superior to other QP solvers in terms of computational time and solvability
  problem_->setup(
      std::vector<OmgCore::Taskset>{taskset_},
      std::vector<OmgCore::QpSolverType>{OmgCore::QpSolverType::JRLQP});

  // Copy problem rbc_arr to member rb_arr to synchronize them
  rbc_arr_ = problem_->rbcArr();

  // Overwrite joint range to restrict joints to be used
  // Be carefull that this overwrites original robot
  // This becomes unnecessary when optmotiongen supports joint selection
  const auto& rb = rb_arr_[0];
  for (const auto& joint : rb->joints()) {
    if (std::find(joint_name_list_.begin(), joint_name_list_.end(), joint.name())
        != joint_name_list_.end()) {
      continue;
    }

    int joint_idx = rb->jointIndexByName(joint.name());
    std::fill(rb->jposs_min_[joint_idx].begin(), rb->jposs_min_[joint_idx].end(), 0);
    std::fill(rb->jposs_max_[joint_idx].begin(), rb->jposs_max_[joint_idx].end(), 0);
  }

  // Get upper and lower position of bounding box in configuration space
  sample_list_.resize(config_.bbox_sample_num);
  reachability_list_.resize(config_.bbox_sample_num);

  RmapSampling<SamplingSpaceType>::setupSampling();
  for (int i = 0; i < config_.bbox_sample_num; i++) {
    RmapSampling<SamplingSpaceType>::sampleOnce(i);
  }

  Eigen::Vector3d upper_body_pos = Eigen::Vector3d::Constant(-1e10);
  Eigen::Vector3d lower_body_pos = Eigen::Vector3d::Constant(1e10);
  for (const SampleType& sample : sample_list_) {
    const Eigen::Vector3d& cloud_pos = sampleToCloudPos<SamplingSpaceType>(sample);
    upper_body_pos = upper_body_pos.cwiseMax(cloud_pos);
    lower_body_pos = lower_body_pos.cwiseMin(cloud_pos);
  }

  // Calculate coefficient and offset to make random position
  body_pos_coeff_ = config_.bbox_padding_rate * (upper_body_pos - lower_body_pos) / 2;
  body_pos_offset_ = (upper_body_pos + lower_body_pos) / 2;
}

template <SamplingSpace SamplingSpaceType>
void RmapSamplingIK<SamplingSpaceType>::setupCollisionTask()
{
  // Since robot_convex_path needs to resolve the ROS package path, it is obtained by rosparam instead of mc_rtc configuration
  std::string robot_convex_path;
  nh_.getParam("robot_convex_path", robot_convex_path);

  collision_task_list_.clear();
  for (const auto& body_names : config_.collision_body_names_list) {
    OmgCore::Twin<std::shared_ptr<sch::S_Object>> sch_objs;
    for (auto i : {0, 1}) {
      sch_objs[i] = OmgCore::loadSchPolyhedron(robot_convex_path + body_names[i] + "_mesh-ch.txt");
    }
    collision_task_list_.push_back(
        std::make_shared<OmgCore::CollisionTask>(
            std::make_shared<OmgCore::CollisionFunc>(
                rb_arr_,
                OmgCore::Twin<int>{0, 0},
                body_names,
                sch_objs),
            0.05));
  }
}

template <SamplingSpace SamplingSpaceType>
void RmapSamplingIK<SamplingSpaceType>::sampleOnce(int sample_idx)
{
  // Set IK target
  if constexpr (SamplingSpaceType == SamplingSpace::R2 ||
                SamplingSpaceType == SamplingSpace::SO2 ||
                SamplingSpaceType == SamplingSpace::SE2) {
      body_task_->target().translation().head<2>() =
          body_pos_coeff_.head<2>().cwiseProduct(Eigen::Vector2d::Random()) + body_pos_offset_.head<2>();
      body_task_->target().translation().z() = 0;
      body_task_->target().rotation() = Eigen::AngleAxisd(
          M_PI * Eigen::Matrix<double, 1, 1>::Random()[0],
          Eigen::Vector3d::UnitZ()).toRotationMatrix();
    } else {
    body_task_->target().translation() =
        body_pos_coeff_.cwiseProduct(Eigen::Vector3d::Random()) + body_pos_offset_;
    body_task_->target().rotation() = Eigen::Quaterniond::UnitRandom().toRotationMatrix();
  }

  bool reachability = false;

  for (int i = 0; i < config_.ik_trial_num; i++) {
    const auto& rb = rb_arr_[0];
    const auto& rbc = rbc_arr_[0];

    if (i == 0) {
      // Set zero configuration
      rbc->zero(*rb);
    } else {
      // Set random configuration
      Eigen::VectorXd joint_pos =
          joint_pos_coeff_.cwiseProduct(Eigen::VectorXd::Random(joint_name_list_.size())) + joint_pos_offset_;
      for (size_t j = 0; j < joint_name_list_.size(); j++) {
        rbc->q[joint_idx_list_[j]][0] = joint_pos[j];
      }
    }
    rbd::forwardKinematics(*rb, *rbc);

    // Solve IK
    problem_->run(config_.ik_loop_num);
    taskset_.update(rb_arr_, rbc_arr_, aux_rb_arr_);

    if (taskset_.errorSquaredNorm() < std::pow(config_.ik_error_thre, 2)) {
      reachability = true;
      break;
    }
  }

  // Append new sample to sample list
  const SampleType& sample = poseToSample<SamplingSpaceType>(body_task_->target());
  sample_list_[sample_idx] = sample;
  reachability_list_[sample_idx] = reachability;
  if (reachability) {
    reachable_cloud_msg_.points.push_back(OmgCore::toPoint32Msg(sampleToCloudPos<SamplingSpaceType>(sample)));
  } else {
    unreachable_cloud_msg_.points.push_back(OmgCore::toPoint32Msg(sampleToCloudPos<SamplingSpaceType>(sample)));
  }
}

template <SamplingSpace SamplingSpaceType>
void RmapSamplingIK<SamplingSpaceType>::publish()
{
  RmapSampling<SamplingSpaceType>::publish();

  // Publish collision marker
  visualization_msgs::MarkerArray marker_arr_msg;

  // delete marker
  visualization_msgs::Marker del_marker;
  del_marker.action = visualization_msgs::Marker::DELETEALL;
  del_marker.header.frame_id = "world";
  del_marker.id = marker_arr_msg.markers.size();
  marker_arr_msg.markers.push_back(del_marker);

  // point and line list marker connecting the closest points
  visualization_msgs::Marker closest_points_marker;
  closest_points_marker.header.frame_id = "world";
  closest_points_marker.ns = "closest_points";
  closest_points_marker.id = marker_arr_msg.markers.size();
  closest_points_marker.type = visualization_msgs::Marker::SPHERE_LIST;
  closest_points_marker.color = OmgCore::toColorRGBAMsg({1, 0, 0, 1});
  closest_points_marker.scale = OmgCore::toVector3Msg({0.02, 0.02, 0.02}); // sphere size
  closest_points_marker.pose.orientation = OmgCore::toQuaternionMsg({0, 0, 0, 1});
  visualization_msgs::Marker closest_lines_marker;
  closest_lines_marker.header.frame_id = "world";
  closest_lines_marker.ns = "closest_lines";
  closest_lines_marker.id = marker_arr_msg.markers.size();
  closest_lines_marker.type = visualization_msgs::Marker::LINE_LIST;
  closest_lines_marker.color = OmgCore::toColorRGBAMsg({1, 0, 0, 1});
  closest_lines_marker.scale.x = 0.01; // line width
  closest_lines_marker.pose.orientation = OmgCore::toQuaternionMsg({0, 0, 0, 1});
  for (const auto& collision_task : collision_task_list_) {
    for (auto i : {0, 1}) {
      closest_points_marker.points.push_back(
          OmgCore::toPointMsg(collision_task->func()->closest_points_[i]));
      closest_lines_marker.points.push_back(
          OmgCore::toPointMsg(collision_task->func()->closest_points_[i]));
    }
  }
  marker_arr_msg.markers.push_back(closest_points_marker);
  marker_arr_msg.markers.push_back(closest_lines_marker);
  collision_marker_pub_.publish(marker_arr_msg);
}

std::shared_ptr<RmapSamplingBase> DiffRmap::createRmapSamplingIK(
    SamplingSpace sampling_space,
    const std::shared_ptr<OmgCore::Robot>& rb,
    const std::string& body_name,
    const std::vector<std::string>& joint_name_list)
{
  if (sampling_space == SamplingSpace::R2) {
    return std::make_shared<RmapSamplingIK<SamplingSpace::R2>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SO2) {
    return std::make_shared<RmapSamplingIK<SamplingSpace::SO2>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SE2) {
    return std::make_shared<RmapSamplingIK<SamplingSpace::SE2>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::R3) {
    return std::make_shared<RmapSamplingIK<SamplingSpace::R3>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SO3) {
    return std::make_shared<RmapSamplingIK<SamplingSpace::SO3>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SE3) {
    return std::make_shared<RmapSamplingIK<SamplingSpace::SE3>>(rb, body_name, joint_name_list);
  } else {
    mc_rtc::log::error_and_throw<std::runtime_error>(
        "[createRmapSamplingIK] Unsupported SamplingSpace: {}", std::to_string(sampling_space));
  }
}

// Declare template specialized class
// See https://stackoverflow.com/a/8752879
template class RmapSamplingIK<SamplingSpace::R2>;
template class RmapSamplingIK<SamplingSpace::SO2>;
template class RmapSamplingIK<SamplingSpace::SE2>;
template class RmapSamplingIK<SamplingSpace::R3>;
template class RmapSamplingIK<SamplingSpace::SO3>;
template class RmapSamplingIK<SamplingSpace::SE3>;
