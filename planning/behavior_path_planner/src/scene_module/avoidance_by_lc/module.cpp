// Copyright 2023 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "behavior_path_planner/scene_module/avoidance_by_lc/module.hpp"

#include "behavior_path_planner/scene_module/scene_module_interface.hpp"
#include "behavior_path_planner/scene_module/scene_module_visitor.hpp"
#include "behavior_path_planner/turn_signal_decider.hpp"
#include "behavior_path_planner/util/avoidance/util.hpp"
#include "behavior_path_planner/util/lane_change/util.hpp"
#include "behavior_path_planner/util/path_utils.hpp"
#include "behavior_path_planner/util/utils.hpp"

#include <lanelet2_extension/utility/message_conversion.hpp>
#include <lanelet2_extension/utility/utilities.hpp>
#include <tier4_autoware_utils/tier4_autoware_utils.hpp>

#include "tier4_planning_msgs/msg/detail/lane_change_debug_msg_array__struct.hpp"
#include <autoware_auto_perception_msgs/msg/object_classification.hpp>
#include <autoware_auto_vehicle_msgs/msg/turn_indicators_command.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace behavior_path_planner
{

using autoware_auto_perception_msgs::msg::ObjectClassification;
using motion_utils::calcSignedArcLength;
using motion_utils::findNearestIndex;
using motion_utils::findNearestSegmentIndex;
using tier4_autoware_utils::calcDistance2d;
using tier4_autoware_utils::calcLateralDeviation;
using tier4_autoware_utils::toHexString;

AvoidanceByLCModule::AvoidanceByLCModule(
  const std::string & name, rclcpp::Node & node,
  const std::shared_ptr<AvoidanceByLCParameters> parameters,
  const std::unordered_map<std::string, std::shared_ptr<RTCInterface>> & rtc_interface_ptr_map)
: SceneModuleInterface{name, node, rtc_interface_ptr_map}, parameters_{parameters}
{
  steering_factor_interface_ptr_ =
    std::make_unique<SteeringFactorInterface>(&node, "avoidance_by_lane_change");
}

void AvoidanceByLCModule::processOnEntry()
{
#ifndef USE_OLD_ARCHITECTURE
  waitApproval();
#endif
  current_lane_change_state_ = LaneChangeStates::Normal;
  updateLaneChangeStatus();
}

void AvoidanceByLCModule::processOnExit()
{
  resetParameters();
}

bool AvoidanceByLCModule::isExecutionRequested() const
{
  if (current_state_ == ModuleStatus::RUNNING) {
    return true;
  }

#ifdef USE_OLD_ARCHITECTURE
  const auto current_lanes = util::getCurrentLanes(planner_data_);
#else
  const auto current_lanes =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif
  const auto lane_change_lanes = getLaneChangeLanes(current_lanes, lane_change_lane_length_);

  LaneChangePath selected_path;
  const auto [found_valid_path, found_safe_path] =
    getSafePath(lane_change_lanes, check_distance_, selected_path);

  if (!found_valid_path) {
    return false;
  }

  const auto object_num = avoidance_data_.target_objects.size();
  if (parameters_->execute_object_num > object_num) {
    return false;
  }

  const auto to_front_object_distance = avoidance_data_.target_objects.front().longitudinal;
  if (parameters_->execute_object_longitudinal_margin > to_front_object_distance) {
    return false;
  }

  const auto to_lane_change_end_distance = calcSignedArcLength(
    selected_path.path.points, getEgoPose().position, selected_path.shift_line.end.position);
  const auto lane_change_finish_before_object =
    to_front_object_distance > to_lane_change_end_distance;
  if (
    !lane_change_finish_before_object &&
    parameters_->execute_only_when_lane_change_finish_before_object) {
    return false;
  }

  return true;
}

bool AvoidanceByLCModule::isExecutionReady() const
{
  if (current_state_ == ModuleStatus::RUNNING) {
    return true;
  }

#ifdef USE_OLD_ARCHITECTURE
  const auto current_lanes = util::getCurrentLanes(planner_data_);
#else
  const auto current_lanes =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif
  const auto lane_change_lanes = getLaneChangeLanes(current_lanes, lane_change_lane_length_);

  LaneChangePath selected_path;
  const auto [found_valid_path, found_safe_path] =
    getSafePath(lane_change_lanes, check_distance_, selected_path);

  return found_safe_path;
}

void AvoidanceByLCModule::updateData()
{
  debug_data_ = DebugData();
  avoidance_data_ = calcAvoidancePlanningData(debug_data_);

  updateRegisteredObject(
    registered_objects_, avoidance_data_.target_objects, parameters_->avoidance);
  compensateDetectionLost(
    registered_objects_, avoidance_data_.target_objects, avoidance_data_.other_objects);

  std::sort(
    avoidance_data_.target_objects.begin(), avoidance_data_.target_objects.end(),
    [](auto a, auto b) { return a.longitudinal < b.longitudinal; });
}

AvoidancePlanningData AvoidanceByLCModule::calcAvoidancePlanningData(DebugData & debug) const
{
  AvoidancePlanningData data;

  // reference pose
  const auto reference_pose = getEgoPose();
  data.reference_pose = reference_pose;

  data.reference_path = util::resamplePathWithSpline(
    *getPreviousModuleOutput().path, parameters_->avoidance->resample_interval_for_planning);

  const size_t nearest_segment_index =
    findNearestSegmentIndex(data.reference_path.points, data.reference_pose.position);
  data.ego_closest_path_index =
    std::min(nearest_segment_index + 1, data.reference_path.points.size() - 1);

  // arclength from ego pose (used in many functions)
  data.arclength_from_ego = util::calcPathArcLengthArray(
    data.reference_path, 0, data.reference_path.points.size(),
    calcSignedArcLength(data.reference_path.points, getEgoPosition(), 0));

  // lanelet info
#ifdef USE_OLD_ARCHITECTURE
  data.current_lanelets = util::getCurrentLanes(planner_data_);
#else
  data.current_lanelets =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif

  // target objects for avoidance
  fillAvoidanceTargetObjects(data, debug);

  return data;
}

void AvoidanceByLCModule::fillAvoidanceTargetObjects(
  AvoidancePlanningData & data, DebugData & debug) const
{
  const auto expanded_lanelets = lanelet::utils::getExpandedLanelets(
    data.current_lanelets, parameters_->avoidance->detection_area_left_expand_dist,
    parameters_->avoidance->detection_area_right_expand_dist * (-1.0));

  const auto [object_within_target_lane, object_outside_target_lane] =
    util::separateObjectsByLanelets(*planner_data_->dynamic_object, expanded_lanelets);

  for (const auto & object : object_outside_target_lane.objects) {
    ObjectData other_object;
    other_object.object = object;
    other_object.reason = "OutOfTargetArea";
    data.other_objects.push_back(other_object);
  }

  ObjectDataArray objects;
  for (const auto & object : object_within_target_lane.objects) {
    objects.push_back(createObjectData(data, object));
  }

  filterTargetObjects(objects, data, debug, planner_data_, parameters_->avoidance);
}

ObjectData AvoidanceByLCModule::createObjectData(
  const AvoidancePlanningData & data, const PredictedObject & object) const
{
  using boost::geometry::return_centroid;

  const auto & path_points = data.reference_path.points;
  const auto & object_pose = object.kinematics.initial_pose_with_covariance.pose;
  const auto object_closest_index = findNearestIndex(path_points, object_pose.position);
  const auto object_closest_pose = path_points.at(object_closest_index).point.pose;

  ObjectData object_data{};

  object_data.object = object;

  // Calc envelop polygon.
  fillObjectEnvelopePolygon(
    object_data, registered_objects_, object_closest_pose, parameters_->avoidance);

  // calc object centroid.
  object_data.centroid = return_centroid<Point2d>(object_data.envelope_poly);

  // calc longitudinal distance from ego to closest target object footprint point.
  fillLongitudinalAndLengthByClosestEnvelopeFootprint(
    data.reference_path, getEgoPosition(), object_data);

  // Calc moving time.
  fillObjectMovingTime(object_data, stopped_objects_, parameters_->avoidance);

  // Calc lateral deviation from path to target object.
  object_data.lateral = calcLateralDeviation(object_closest_pose, object_pose.position);

  // Find the footprint point closest to the path, set to object_data.overhang_distance.
  object_data.overhang_dist = calcEnvelopeOverhangDistance(
    object_data, object_closest_pose, object_data.overhang_pose.position);

  // Check whether the the ego should avoid the object.
  const auto & vehicle_width = planner_data_->parameters.vehicle_width;
  const auto safety_margin =
    0.5 * vehicle_width + parameters_->avoidance->lateral_passable_safety_buffer;
  object_data.avoid_required =
    (isOnRight(object_data) && std::abs(object_data.overhang_dist) < safety_margin) ||
    (!isOnRight(object_data) && object_data.overhang_dist < safety_margin);

  return object_data;
}

ModuleStatus AvoidanceByLCModule::updateState()
{
  RCLCPP_DEBUG(getLogger(), "AVOIDANCE_BY_LC updateState");
  if (!isValidPath()) {
    current_state_ = ModuleStatus::SUCCESS;
    return current_state_;
  }

  if (isWaitingApproval()) {
    const auto object_num = avoidance_data_.target_objects.size();
    if (parameters_->execute_object_num > object_num) {
      current_state_ = ModuleStatus::SUCCESS;
      return current_state_;
    }

    const auto to_front_object_distance = avoidance_data_.target_objects.front().longitudinal;
    if (parameters_->execute_object_longitudinal_margin > to_front_object_distance) {
      current_state_ = ModuleStatus::FAILURE;
      return current_state_;
    }

    const auto to_lane_change_end_distance = calcSignedArcLength(
      status_.lane_change_path.path.points, getEgoPose().position,
      status_.lane_change_path.shift_line.end.position);
    const auto lane_change_finish_before_object =
      to_front_object_distance > to_lane_change_end_distance;
    if (
      !lane_change_finish_before_object &&
      parameters_->execute_only_when_lane_change_finish_before_object) {
      current_state_ = ModuleStatus::FAILURE;
      return current_state_;
    }
  }

  const auto is_within_current_lane = util::lane_change::isEgoWithinOriginalLane(
    status_.current_lanes, getEgoPose(), planner_data_->parameters);
  if (isAbortState() && !is_within_current_lane) {
    current_state_ = ModuleStatus::RUNNING;
    return current_state_;
  }

  if (isAbortConditionSatisfied()) {
    if ((isNearEndOfLane() && isCurrentVelocityLow()) || !is_within_current_lane) {
      current_state_ = ModuleStatus::RUNNING;
      return current_state_;
    }

    current_state_ = ModuleStatus::FAILURE;
    return current_state_;
  }

  if (hasFinishedLaneChange()) {
    current_state_ = ModuleStatus::SUCCESS;
    return current_state_;
  }
  current_state_ = ModuleStatus::RUNNING;
  return current_state_;
}

BehaviorModuleOutput AvoidanceByLCModule::plan()
{
  resetPathCandidate();
  resetPathReference();
  is_activated_ = isActivated();

  PathWithLaneId path = status_.lane_change_path.path;
  if (!isValidPath(path)) {
    status_.is_valid_path = false;
    return BehaviorModuleOutput{};
  } else {
    status_.is_valid_path = true;
  }

  if ((is_abort_condition_satisfied_ && isNearEndOfLane() && isCurrentVelocityLow())) {
    const auto stop_point = util::insertStopPoint(0.1, path);
  }

  if (isAbortState()) {
    resetPathIfAbort();
    if (is_activated_) {
      path = abort_path_->path;
    }
  }

  generateExtendedDrivableArea(path);

  BehaviorModuleOutput output;
  output.path = std::make_shared<PathWithLaneId>(path);
#ifdef USE_OLD_ARCHITECTURE
  path_reference_ = getPreviousModuleOutput().reference_path;
  prev_approved_path_ = path;
#else
  const auto reference_path =
    util::getCenterLinePathFromRootLanelet(status_.lane_change_lanes.front(), planner_data_);
  output.reference_path = std::make_shared<PathWithLaneId>(reference_path);
  path_reference_ = std::make_shared<PathWithLaneId>(reference_path);
  prev_approved_path_ = *getPreviousModuleOutput().path;
#endif
  updateOutputTurnSignal(output);

  updateSteeringFactorPtr(output);
  clearWaitingApproval();

  return output;
}

void AvoidanceByLCModule::resetPathIfAbort()
{
  if (!is_abort_approval_requested_) {
#ifdef USE_OLD_ARCHITECTURE
    const auto lateral_shift = util::lane_change::getLateralShift(*abort_path_);
    if (lateral_shift > 0.0) {
      removePreviousRTCStatusRight();
      uuid_map_.at("right") = generateUUID();
    } else if (lateral_shift < 0.0) {
      removePreviousRTCStatusLeft();
      uuid_map_.at("left") = generateUUID();
    }
#else
    removeRTCStatus();
#endif
    RCLCPP_DEBUG(getLogger(), "[abort] uuid is reset to request abort approval.");
    is_abort_approval_requested_ = true;
    is_abort_path_approved_ = false;
    return;
  }

  if (isActivated()) {
    RCLCPP_DEBUG(getLogger(), "[abort] isActivated() is true. set is_abort_path_approved to true.");
    is_abort_path_approved_ = true;
    clearWaitingApproval();
  } else {
    RCLCPP_DEBUG(getLogger(), "[abort] isActivated() is False.");
    is_abort_path_approved_ = false;
    waitApproval();
  }
}

CandidateOutput AvoidanceByLCModule::planCandidate() const
{
  CandidateOutput output;

  LaneChangePath selected_path;
  // Get lane change lanes
#ifdef USE_OLD_ARCHITECTURE
  const auto current_lanes = util::getCurrentLanes(planner_data_);
#else
  const auto current_lanes =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif
  const auto lane_change_lanes = getLaneChangeLanes(current_lanes, lane_change_lane_length_);

  if (lane_change_lanes.empty()) {
    return output;
  }

#ifdef USE_OLD_ARCHITECTURE
  [[maybe_unused]] const auto [found_valid_path, found_safe_path] =
    getSafePath(lane_change_lanes, check_distance_, selected_path);
#else
  selected_path = status_.lane_change_path;
#endif

  selected_path.path.header = planner_data_->route_handler->getRouteHeader();

  if (isAbortState()) {
    selected_path = *abort_path_;
  }

  if (selected_path.path.points.empty()) {
    return output;
  }

  output.path_candidate = selected_path.path;
  output.lateral_shift = util::lane_change::getLateralShift(selected_path);
  output.start_distance_to_path_change = motion_utils::calcSignedArcLength(
    selected_path.path.points, getEgoPose().position, selected_path.shift_line.start.position);
  output.finish_distance_to_path_change = motion_utils::calcSignedArcLength(
    selected_path.path.points, getEgoPose().position, selected_path.shift_line.end.position);

  updateSteeringFactorPtr(output, selected_path);
  return output;
}

BehaviorModuleOutput AvoidanceByLCModule::planWaitingApproval()
{
#ifdef USE_OLD_ARCHITECTURE
  const auto is_within_current_lane = util::lane_change::isEgoWithinOriginalLane(
    status_.current_lanes, getEgoPose(), planner_data_->parameters);
  if (is_within_current_lane) {
    prev_approved_path_ = getReferencePath();
  }
#else
  prev_approved_path_ = *getPreviousModuleOutput().path;
#endif
  BehaviorModuleOutput out;
  out.path = std::make_shared<PathWithLaneId>(prev_approved_path_);
  out.reference_path = getPreviousModuleOutput().reference_path;
  out.turn_signal_info = getPreviousModuleOutput().turn_signal_info;

  if (!avoidance_data_.target_objects.empty()) {
    const auto to_front_object_distance = avoidance_data_.target_objects.front().longitudinal;
    const auto lane_change_buffer = planner_data_->parameters.minimum_lane_changing_length;

    boost::optional<Pose> p_insert{};
    insertDecelPoint(
      getEgoPosition(), to_front_object_distance - lane_change_buffer, 0.0, *out.path, p_insert);
  }

#ifndef USE_OLD_ARCHITECTURE
  updateLaneChangeStatus();
#endif

  const auto candidate = planCandidate();
  path_candidate_ = std::make_shared<PathWithLaneId>(candidate.path_candidate);
  path_reference_ = getPreviousModuleOutput().reference_path;
  updateRTCStatus(candidate);
  waitApproval();
  is_abort_path_approved_ = false;

  return out;
}

void AvoidanceByLCModule::updateLaneChangeStatus()
{
#ifdef USE_OLD_ARCHITECTURE
  status_.current_lanes = util::getCurrentLanes(planner_data_);
#else
  status_.current_lanes =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif
  status_.lane_change_lanes = getLaneChangeLanes(status_.current_lanes, lane_change_lane_length_);

  // Find lane change path
  LaneChangePath selected_path;
  const auto [found_valid_path, found_safe_path] =
    getSafePath(status_.lane_change_lanes, check_distance_, selected_path);

  // Update status
  status_.is_safe = found_safe_path;
  status_.lane_change_path = selected_path;
  status_.lane_follow_lane_ids = util::getIds(status_.current_lanes);
  status_.lane_change_lane_ids = util::getIds(status_.lane_change_lanes);

  const auto arclength_start =
    lanelet::utils::getArcCoordinates(status_.lane_change_lanes, getEgoPose());
  status_.start_distance = arclength_start.length;
  status_.lane_change_path.path.header = getRouteHeader();
}

PathWithLaneId AvoidanceByLCModule::getReferencePath() const
{
  PathWithLaneId reference_path;

  const auto & route_handler = planner_data_->route_handler;
  const auto current_pose = getEgoPose();
  const auto & common_parameters = planner_data_->parameters;

  // Set header
  reference_path.header = getRouteHeader();

#ifdef USE_OLD_ARCHITECTURE
  const auto current_lanes = util::getCurrentLanes(planner_data_);
#else
  const auto current_lanes =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif

  if (current_lanes.empty()) {
    return reference_path;
  }

  if (reference_path.points.empty()) {
    reference_path = util::getCenterLinePath(
      *route_handler, current_lanes, current_pose, common_parameters.backward_path_length,
      common_parameters.forward_path_length, common_parameters);
  }

  const int num_lane_change =
    std::abs(route_handler->getNumLaneToPreferredLane(current_lanes.back()));

  reference_path = util::getCenterLinePath(
    *route_handler, current_lanes, current_pose, common_parameters.backward_path_length,
    common_parameters.forward_path_length, common_parameters, 0.0);

  const double lane_change_buffer =
    util::calcLaneChangeBuffer(common_parameters, num_lane_change, 0.0);

  reference_path = util::setDecelerationVelocity(
    *route_handler, reference_path, current_lanes, parameters_->lane_change->prepare_duration,
    lane_change_buffer);

  const auto drivable_lanes = util::generateDrivableLanes(current_lanes);
  const auto shorten_lanes = util::cutOverlappedLanes(reference_path, drivable_lanes);
  const auto expanded_lanes = util::expandLanelets(
    shorten_lanes, parameters_->lane_change->drivable_area_left_bound_offset,
    parameters_->lane_change->drivable_area_right_bound_offset,
    parameters_->lane_change->drivable_area_types_to_skip);
  util::generateDrivableArea(
    reference_path, expanded_lanes, common_parameters.vehicle_length, planner_data_);

  return reference_path;
}

lanelet::ConstLanelets AvoidanceByLCModule::getLaneChangeLanes(
  const lanelet::ConstLanelets & current_lanes, const double lane_change_lane_length) const
{
  lanelet::ConstLanelets lane_change_lanes;
  const auto & route_handler = planner_data_->route_handler;
  const auto minimum_lane_changing_length = planner_data_->parameters.minimum_lane_changing_length;
  const auto prepare_duration = parameters_->lane_change->prepare_duration;
  const auto current_pose = getEgoPose();
  const auto current_twist = getEgoTwist();

  if (current_lanes.empty()) {
    return lane_change_lanes;
  }

  const auto object_num = avoidance_data_.target_objects.size();
  if (object_num < parameters_->execute_object_num) {
    return lane_change_lanes;
  }

  const auto o_front = avoidance_data_.target_objects.front();

  // Get lane change lanes
  lanelet::ConstLanelet current_lane;
  lanelet::utils::query::getClosestLanelet(current_lanes, current_pose, &current_lane);
  const double lane_change_prepare_length =
    std::max(current_twist.linear.x * prepare_duration, minimum_lane_changing_length);
  lanelet::ConstLanelets current_check_lanes =
    route_handler->getLaneletSequence(current_lane, current_pose, 0.0, lane_change_prepare_length);
  lanelet::ConstLanelet lane_change_lane;

  if (isOnRight(o_front)) {
    for (const auto & lanelet : current_check_lanes) {
      const auto & left_lane = route_handler->getRoutingGraphPtr()->left(lanelet);
      if (left_lane) {
        lane_change_lanes = route_handler->getLaneletSequence(
          left_lane.get(), current_pose, lane_change_lane_length, lane_change_lane_length);
        break;
      }
    }
  } else {
    for (const auto & lanelet : current_check_lanes) {
      const auto & right_lane = route_handler->getRoutingGraphPtr()->right(lanelet);
      if (right_lane) {
        lane_change_lanes = route_handler->getLaneletSequence(
          right_lane.get(), current_pose, lane_change_lane_length, lane_change_lane_length);
        break;
      }
    }
  }

  return lane_change_lanes;
}

std::pair<bool, bool> AvoidanceByLCModule::getSafePath(
  const lanelet::ConstLanelets & lane_change_lanes, const double check_distance,
  LaneChangePath & safe_path) const
{
  const auto & route_handler = planner_data_->route_handler;
  const auto current_pose = getEgoPose();
  const auto current_twist = getEgoTwist();
  const auto & common_parameters = planner_data_->parameters;

#ifdef USE_OLD_ARCHITECTURE
  const auto current_lanes = util::getCurrentLanes(planner_data_);
#else
  const auto current_lanes =
    util::getCurrentLanesFromPath(*getPreviousModuleOutput().reference_path, planner_data_);
#endif

  if (lane_change_lanes.empty()) {
    return std::make_pair(false, false);
  }

  if (avoidance_data_.target_objects.empty()) {
    return std::make_pair(false, false);
  }

  // find candidate paths
  LaneChangePaths valid_paths;
#ifdef USE_OLD_ARCHITECTURE
  const auto found_safe_path = util::lane_change::getLaneChangePaths(
    *route_handler, current_lanes, lane_change_lanes, current_pose, current_twist,
    planner_data_->dynamic_object, common_parameters, *parameters_->lane_change, check_distance,
    &valid_paths, &object_debug_);
#else
  const auto o_front = avoidance_data_.target_objects.front();
  const auto direction = isOnRight(o_front) ? Direction::LEFT : Direction::RIGHT;
  const auto found_safe_path = util::lane_change::getLaneChangePaths(
    *getPreviousModuleOutput().path, *route_handler, current_lanes, lane_change_lanes, current_pose,
    current_twist, planner_data_->dynamic_object, common_parameters, *parameters_->lane_change,
    check_distance, direction, &valid_paths, &object_debug_);
#endif
  debug_valid_path_ = valid_paths;

  if (parameters_->lane_change->publish_debug_marker) {
    setObjectDebugVisualization();
  } else {
    debug_marker_.markers.clear();
  }

  if (valid_paths.empty()) {
    return {false, false};
  }

  if (found_safe_path) {
    safe_path = valid_paths.back();
  } else {
    // force candidate
    safe_path = valid_paths.front();
  }

  return {true, found_safe_path};
}

bool AvoidanceByLCModule::isSafe() const
{
  return status_.is_safe;
}

bool AvoidanceByLCModule::isValidPath() const
{
  return status_.is_valid_path;
}

bool AvoidanceByLCModule::isValidPath(const PathWithLaneId & path) const
{
  const auto & route_handler = planner_data_->route_handler;

  // check lane departure
  const auto drivable_lanes = util::lane_change::generateDrivableLanes(
    *route_handler, util::extendLanes(route_handler, status_.current_lanes),
    util::extendLanes(route_handler, status_.lane_change_lanes));
  const auto expanded_lanes = util::expandLanelets(
    drivable_lanes, parameters_->lane_change->drivable_area_left_bound_offset,
    parameters_->lane_change->drivable_area_right_bound_offset);
  const auto lanelets = util::transformToLanelets(expanded_lanes);

  // check path points are in any lanelets
  for (const auto & point : path.points) {
    bool is_in_lanelet = false;
    for (const auto & lanelet : lanelets) {
      if (lanelet::utils::isInLanelet(point.point.pose, lanelet)) {
        is_in_lanelet = true;
        break;
      }
    }
    if (!is_in_lanelet) {
      RCLCPP_WARN_STREAM_THROTTLE(getLogger(), *clock_, 1000, "path is out of lanes");
      return false;
    }
  }

  // check relative angle
  if (!util::checkPathRelativeAngle(path, M_PI)) {
    RCLCPP_WARN_STREAM_THROTTLE(getLogger(), *clock_, 1000, "path relative angle is invalid");
    return false;
  }

  return true;
}

bool AvoidanceByLCModule::isNearEndOfLane() const
{
  const auto & current_pose = getEgoPose();
  const double threshold = util::calcTotalLaneChangeLength(planner_data_->parameters);

  return std::max(0.0, util::getDistanceToEndOfLane(current_pose, status_.current_lanes)) <
         threshold;
}

bool AvoidanceByLCModule::isCurrentVelocityLow() const
{
  constexpr double threshold_ms = 10.0 * 1000 / 3600;
  return util::l2Norm(getEgoTwist().linear) < threshold_ms;
}

bool AvoidanceByLCModule::isAbortConditionSatisfied()
{
  is_abort_condition_satisfied_ = false;
  current_lane_change_state_ = LaneChangeStates::Normal;

  if (!parameters_->lane_change->enable_cancel_lane_change) {
    return false;
  }

  if (!is_activated_) {
    return false;
  }

  Pose ego_pose_before_collision;
  const auto is_path_safe = isApprovedPathSafe(ego_pose_before_collision);

  if (!is_path_safe) {
    const auto & common_parameters = planner_data_->parameters;
    const bool is_within_original_lane = util::lane_change::isEgoWithinOriginalLane(
      status_.current_lanes, getEgoPose(), common_parameters);

    if (is_within_original_lane) {
      current_lane_change_state_ = LaneChangeStates::Cancel;
      return true;
    }

    // check abort enable flag
    RCLCPP_WARN_STREAM_THROTTLE(
      getLogger(), *clock_, 1000,
      "DANGER!!! Path is not safe anymore, but it is too late to CANCEL! Please be cautious");

    if (!parameters_->lane_change->enable_abort_lane_change) {
      current_lane_change_state_ = LaneChangeStates::Stop;
      return false;
    }

    const auto found_abort_path = util::lane_change::getAbortPaths(
      planner_data_, status_.lane_change_path, ego_pose_before_collision, common_parameters,
      *parameters_->lane_change);

    if (!found_abort_path && !is_abort_path_approved_) {
      current_lane_change_state_ = LaneChangeStates::Stop;
      return true;
    }

    current_lane_change_state_ = LaneChangeStates::Abort;

    if (!is_abort_path_approved_) {
      abort_path_ = std::make_shared<LaneChangePath>(*found_abort_path);
    }

    return true;
  }

  return false;
}

bool AvoidanceByLCModule::isAbortState() const
{
  if (!parameters_->lane_change->enable_abort_lane_change) {
    return false;
  }

  if (current_lane_change_state_ != LaneChangeStates::Abort) {
    return false;
  }

  if (!abort_path_) {
    return false;
  }

  RCLCPP_WARN_STREAM_THROTTLE(
    getLogger(), *clock_, 1000,
    "DANGER!!! Lane change transition to ABORT state, return path will be computed!");
  return true;
}

bool AvoidanceByLCModule::isAvoidancePlanRunning() const
{
  constexpr double AVOIDING_SHIFT_THR = 0.1;

  const auto & current_pose = getEgoPose();
  const auto arclength_current =
    lanelet::utils::getArcCoordinates(status_.current_lanes, current_pose);

  return std::abs(arclength_current.distance) > AVOIDING_SHIFT_THR;
}

bool AvoidanceByLCModule::hasFinishedLaneChange() const
{
  const auto & current_pose = getEgoPose();
  const auto arclength_current =
    lanelet::utils::getArcCoordinates(status_.lane_change_lanes, current_pose);
  const double travel_distance = arclength_current.length - status_.start_distance;
  const double finish_distance = status_.lane_change_path.length.sum() +
                                 parameters_->lane_change->lane_change_finish_judge_buffer;
  return travel_distance > finish_distance;
}

void AvoidanceByLCModule::setObjectDebugVisualization() const
{
  using marker_utils::lane_change_markers::showAllValidLaneChangePath;
  using marker_utils::lane_change_markers::showLerpedPose;
  using marker_utils::lane_change_markers::showObjectInfo;
  using marker_utils::lane_change_markers::showPolygon;
  using marker_utils::lane_change_markers::showPolygonPose;

  debug_marker_.markers.clear();
  const auto add = [this](const MarkerArray & added) {
    tier4_autoware_utils::appendMarkerArray(added, &debug_marker_);
  };

  add(showObjectInfo(object_debug_, "object_debug_info"));
  add(showLerpedPose(object_debug_, "lerp_pose_before_true"));
  add(showPolygonPose(object_debug_, "expected_pose"));
  add(showPolygon(object_debug_, "lerped_polygon"));
  add(showAllValidLaneChangePath(debug_valid_path_, "lane_change_valid_paths"));
}

std::shared_ptr<LaneChangeDebugMsgArray> AvoidanceByLCModule::get_debug_msg_array() const
{
  LaneChangeDebugMsgArray debug_msg_array;
  debug_msg_array.lane_change_info.reserve(object_debug_.size());
  for (const auto & [uuid, debug_data] : object_debug_) {
    LaneChangeDebugMsg debug_msg;
    debug_msg.object_id = uuid;
    debug_msg.allow_lane_change = debug_data.allow_lane_change;
    debug_msg.is_front = debug_data.is_front;
    debug_msg.relative_distance = debug_data.relative_to_ego;
    debug_msg.failed_reason = debug_data.failed_reason;
    debug_msg.velocity = util::l2Norm(debug_data.object_twist.linear);
    debug_msg_array.lane_change_info.push_back(debug_msg);
  }
  lane_change_debug_msg_array_ = debug_msg_array;

  lane_change_debug_msg_array_.header.stamp = clock_->now();
  return std::make_shared<LaneChangeDebugMsgArray>(lane_change_debug_msg_array_);
}

void AvoidanceByLCModule::updateSteeringFactorPtr(const BehaviorModuleOutput & output)
{
  const auto turn_signal_info = output.turn_signal_info;
  const auto current_pose = getEgoPose();
  const double start_distance = motion_utils::calcSignedArcLength(
    output.path->points, current_pose.position, status_.lane_change_path.shift_line.start.position);
  const double finish_distance = motion_utils::calcSignedArcLength(
    output.path->points, current_pose.position, status_.lane_change_path.shift_line.end.position);

  const uint16_t steering_factor_direction =
    std::invoke([this, &start_distance, &finish_distance, &turn_signal_info]() {
      if (turn_signal_info.turn_signal.command == TurnIndicatorsCommand::ENABLE_LEFT) {
        waitApprovalLeft(start_distance, finish_distance);
        return SteeringFactor::LEFT;
      }
      if (turn_signal_info.turn_signal.command == TurnIndicatorsCommand::ENABLE_RIGHT) {
        waitApprovalRight(start_distance, finish_distance);
        return SteeringFactor::RIGHT;
      }
      return SteeringFactor::UNKNOWN;
    });

  // TODO(tkhmy) add handle status TRYING
  steering_factor_interface_ptr_->updateSteeringFactor(
    {status_.lane_change_path.shift_line.start, status_.lane_change_path.shift_line.end},
    {start_distance, finish_distance}, SteeringFactor::LANE_CHANGE, steering_factor_direction,
    SteeringFactor::TURNING, "");
}

void AvoidanceByLCModule::updateSteeringFactorPtr(
  const CandidateOutput & output, const LaneChangePath & selected_path) const
{
  const uint16_t steering_factor_direction = std::invoke([&output]() {
    if (output.lateral_shift > 0.0) {
      return SteeringFactor::LEFT;
    }
    return SteeringFactor::RIGHT;
  });

  steering_factor_interface_ptr_->updateSteeringFactor(
    {selected_path.shift_line.start, selected_path.shift_line.end},
    {output.start_distance_to_path_change, output.finish_distance_to_path_change},
    SteeringFactor::LANE_CHANGE, steering_factor_direction, SteeringFactor::APPROACHING, "");
}

Twist AvoidanceByLCModule::getEgoTwist() const
{
  return planner_data_->self_odometry->twist.twist;
}

std_msgs::msg::Header AvoidanceByLCModule::getRouteHeader() const
{
  return planner_data_->route_handler->getRouteHeader();
}

void AvoidanceByLCModule::generateExtendedDrivableArea(PathWithLaneId & path)
{
  const auto & common_parameters = planner_data_->parameters;
  const auto & route_handler = planner_data_->route_handler;
  const auto drivable_lanes = util::lane_change::generateDrivableLanes(
    *route_handler, status_.current_lanes, status_.lane_change_lanes);
  const auto shorten_lanes = util::cutOverlappedLanes(path, drivable_lanes);
  const auto expanded_lanes = util::expandLanelets(
    shorten_lanes, parameters_->lane_change->drivable_area_left_bound_offset,
    parameters_->lane_change->drivable_area_right_bound_offset);
  util::generateDrivableArea(path, expanded_lanes, common_parameters.vehicle_length, planner_data_);
}

bool AvoidanceByLCModule::isApprovedPathSafe(Pose & ego_pose_before_collision) const
{
  const auto current_pose = getEgoPose();
  const auto current_twist = getEgoTwist();
  const auto & dynamic_objects = planner_data_->dynamic_object;
  const auto & common_parameters = planner_data_->parameters;
  const auto & lane_change_parameters = parameters_->lane_change;
  const auto & route_handler = planner_data_->route_handler;
  const auto & path = status_.lane_change_path;

  // get lanes used for detection
  const auto check_lanes = util::lane_change::getExtendedTargetLanesForCollisionCheck(
    *route_handler, path.target_lanelets.front(), current_pose, check_distance_);

  std::unordered_map<std::string, CollisionCheckDebug> debug_data;
  const auto lateral_buffer =
    util::lane_change::calcLateralBufferForFiltering(common_parameters.vehicle_width);
  const auto dynamic_object_indices = util::lane_change::filterObjectIndices(
    {path}, *dynamic_objects, check_lanes, current_pose, common_parameters.forward_path_length,
    *lane_change_parameters, lateral_buffer);

  return util::lane_change::isLaneChangePathSafe(
    path, dynamic_objects, dynamic_object_indices, current_pose, current_twist, common_parameters,
    *parameters_->lane_change, common_parameters.expected_front_deceleration_for_abort,
    common_parameters.expected_rear_deceleration_for_abort, ego_pose_before_collision, debug_data,
    status_.lane_change_path.acceleration);
}

void AvoidanceByLCModule::updateOutputTurnSignal(BehaviorModuleOutput & output)
{
  const auto turn_signal_info = util::getPathTurnSignal(
    status_.current_lanes, status_.lane_change_path.shifted_path,
    status_.lane_change_path.shift_line, getEgoPose(), getEgoTwist().linear.x,
    planner_data_->parameters);
  output.turn_signal_info.turn_signal.command = turn_signal_info.first.command;

  util::lane_change::get_turn_signal_info(status_.lane_change_path, &output.turn_signal_info);
}

void AvoidanceByLCModule::resetParameters()
{
  is_abort_path_approved_ = false;
  is_abort_approval_requested_ = false;
  current_lane_change_state_ = LaneChangeStates::Normal;
  abort_path_ = nullptr;

  object_debug_.clear();
  debug_marker_.markers.clear();
  resetPathCandidate();
  resetPathReference();
}

void AvoidanceByLCModule::acceptVisitor(const std::shared_ptr<SceneModuleVisitor> & visitor) const
{
  if (visitor) {
    visitor->visitAvoidanceByLCModule(this);
  }
}

void SceneModuleVisitor::visitAvoidanceByLCModule(
  [[maybe_unused]] const AvoidanceByLCModule * module) const
{
  // lane_change_visitor_ = module->get_debug_msg_array();
}
}  // namespace behavior_path_planner
