
#include <ftc_local_planner/ftc_planner.h>

#include <pluginlib/class_list_macros.h>
#include "mbf_msgs/ExePathAction.h"

PLUGINLIB_EXPORT_CLASS(ftc_local_planner::FTCPlanner, mbf_costmap_core::CostmapController)

#define RET_SUCCESS 0
#define RET_COLLISION 104
#define RET_BLOCKED 109

namespace ftc_local_planner
{

    FTCPlanner::FTCPlanner()
    {
    }

    void FTCPlanner::initialize(std::string name, tf2_ros::Buffer *tf, costmap_2d::Costmap2DROS *costmap_ros)
    {
        ros::NodeHandle private_nh("~/" + name);

        progress_server = private_nh.advertiseService(
            "planner_get_progress", &FTCPlanner::getProgress, this);

        global_point_pub = private_nh.advertise<geometry_msgs::PoseStamped>("global_point", 1);
        global_plan_pub = private_nh.advertise<nav_msgs::Path>("global_plan", 1, true);
        obstacle_marker_pub = private_nh.advertise<visualization_msgs::Marker>("costmap_marker", 10);

        costmap = costmap_ros;
        costmap_map_ = costmap_ros->getCostmap();
        tf_buffer = tf;

        // Get footprint of the robot and minimum and maximum distance from the center of the robot to its footprint vertices.
        footprint_spec_ = costmap->getRobotFootprint();
        costmap_2d::calculateMinAndMaxDistances(footprint_spec_, robot_inscribed_radius_, robot_circumscribed_radius_);

        costmap_model_ = new base_local_planner::CostmapModel(*costmap_map_);

        // Parameter for dynamic reconfigure
        reconfig_server = new dynamic_reconfigure::Server<FTCPlannerConfig>(private_nh);
        dynamic_reconfigure::Server<FTCPlannerConfig>::CallbackType cb = boost::bind(&FTCPlanner::reconfigureCB, this,
                                                                                     _1, _2);
        reconfig_server->setCallback(cb);

        current_state = PRE_ROTATE;

        // PID Debugging topic
        if (config.debug_pid)
        {
            pubPid = private_nh.advertise<ftc_local_planner::PID>("debug_pid", 1, true);
        }

        // Recovery behavior initialization
        failure_detector_.setBufferLength(std::round(config.oscillation_recovery_min_duration * 10));

        ROS_INFO("FTCLocalPlannerROS: Version 2 Init.");
    }

    void FTCPlanner::reconfigureCB(FTCPlannerConfig &c, uint32_t level)
    {
        if (c.restore_defaults)
        {
            reconfig_server->getConfigDefault(c);
            c.restore_defaults = false;
        }
        config = c;

        // just to be sure
        current_movement_speed = config.speed_slow;

        // set recovery behavior
        failure_detector_.setBufferLength(std::round(config.oscillation_recovery_min_duration * 10));
    }

    bool FTCPlanner::setPlan(const std::vector<geometry_msgs::PoseStamped> &plan)
    {
        current_state = PRE_ROTATE;
        state_entered_time = ros::Time::now();
        is_crashed = false;

        global_plan = plan;
        current_index = 0;
        current_progress = 0.0;

        last_time = ros::Time::now();
        current_movement_speed = config.speed_slow;

        lat_error = 0.0;
        lon_error = 0.0;
        angle_error = 0.0;
        i_lon_error = 0.0;
        i_lat_error = 0.0;
        i_angle_error = 0.0;

        nav_msgs::Path path;

        if (global_plan.size() > 2)
        {
            // duplicate last point
            global_plan.push_back(global_plan.back());
            // give second from last point last oriantation as the point before that
            global_plan[global_plan.size() - 2].pose.orientation = global_plan[global_plan.size() - 3].pose.orientation;
            path.header = plan.front().header;
            path.poses = plan;
        }
        else
        {
            ROS_WARN_STREAM("FTCLocalPlannerROS: Global plan was too short. Need a minimum of 3 poses - Cancelling.");
            current_state = FINISHED;
            state_entered_time = ros::Time::now();
        }
        global_plan_pub.publish(path);

        ROS_INFO_STREAM("FTCLocalPlannerROS: Got new global plan with " << plan.size() << " points.");

        return true;
    }

    FTCPlanner::~FTCPlanner()
    {
        if (reconfig_server != nullptr)
        {
            delete reconfig_server;
            reconfig_server = nullptr;
        }
    }

    double FTCPlanner::distanceLookahead()
    {
        if (global_plan.size() < 2)
        {
            return 0;
        }
        Eigen::Quaternion<double> current_rot(current_control_point.linear());

        Eigen::Affine3d last_straight_point = current_control_point;
        for (uint32_t i = current_index + 1; i < global_plan.size(); i++)
        {
            tf2::fromMsg(global_plan[i].pose, last_straight_point);
            // check, if direction is the same. if so, we add the distance
            Eigen::Quaternion<double> rot2(last_straight_point.linear());
            if (abs(rot2.angularDistance(current_rot)) > config.speed_fast_threshold_angle * (M_PI / 180.0))
            {
                break;
            }
        }

        return (last_straight_point.translation() - current_control_point.translation()).norm();
    }

    uint32_t FTCPlanner::computeVelocityCommands(const geometry_msgs::PoseStamped &pose,
                                                 const geometry_msgs::TwistStamped &velocity,
                                                 geometry_msgs::TwistStamped &cmd_vel, std::string &message)
    {

        ros::Time now = ros::Time::now();
        double dt = now.toSec() - last_time.toSec();
        last_time = now;

        if (is_crashed)
        {
            cmd_vel.twist.linear.x = 0;
            cmd_vel.twist.angular.z = 0;
            return RET_COLLISION;
        }

        if (current_state == FINISHED)
        {
            cmd_vel.twist.linear.x = 0;
            cmd_vel.twist.angular.z = 0;
            return RET_SUCCESS;
        }

        // We're not crashed and not finished.
        // First, we update the control point if needed. This is needed since we need the local_control_point to calculate the next state.
        update_control_point(dt);
        // Then, update the planner state.
        auto new_planner_state = update_planner_state();
        if (new_planner_state != current_state)
        {
            ROS_INFO_STREAM("FTCLocalPlannerROS: Switching to state " << new_planner_state);
            state_entered_time = ros::Time::now();
            current_state = new_planner_state;
        }

        // if (checkCollision(5))
        // {
        //     cmd_vel.twist.linear.x = 0;
        //     cmd_vel.twist.angular.z = 0;
        //     is_crashed = true;
        //     return RET_BLOCKED;
        // }
footprint_spec_ = costmap->getRobotFootprint();
costmap_2d::calculateMinAndMaxDistances(footprint_spec_, robot_inscribed_radius_, robot_circumscribed_radius_);
        if (!isTrajectoryFeasible(costmap_model_, footprint_spec_, robot_inscribed_radius_, robot_circumscribed_radius_, 5))
        {
            cmd_vel.twist.linear.x = 0;
            cmd_vel.twist.angular.z = 0;
            is_crashed = true;
            return RET_BLOCKED;
        }

        // Finally, we calculate the velocity commands.
        calculate_velocity_commands(dt, cmd_vel);

        if (is_crashed)
        {
            cmd_vel.twist.linear.x = 0;
            cmd_vel.twist.angular.z = 0;
            return RET_COLLISION;
        }

        return RET_SUCCESS;
    }

    bool FTCPlanner::isGoalReached(double dist_tolerance, double angle_tolerance)
    {
        return current_state == FINISHED;
    }

    bool FTCPlanner::cancel()
    {
        ROS_WARN_STREAM("FTCLocalPlannerROS: FTC planner was cancelled.");
        current_state = FINISHED;
        state_entered_time = ros::Time::now();
        return true;
    }

    FTCPlanner::PlannerState FTCPlanner::update_planner_state()
    {
        switch (current_state)
        {
        case PRE_ROTATE:
        {
            if (time_in_current_state() > config.goal_timeout)
            {
                ROS_ERROR_STREAM("FTCLocalPlannerROS: Error reaching goal. config.goal_timeout (" << config.goal_timeout << ") reached - Timeout in PRE_ROTATE phase.");
                is_crashed = true;
                return FINISHED;
            }
            if (abs(angle_error) * (180.0 / M_PI) < config.max_goal_angle_error)
            {
                ROS_INFO_STREAM("FTCLocalPlannerROS: PRE_ROTATE finished. Starting following");
                return FOLLOWING;
            }
        }
        break;
        case FOLLOWING:
        {
            double distance = local_control_point.translation().norm();
            // check for crash
            if (distance > config.max_follow_distance)
            {
                ROS_ERROR_STREAM("FTCLocalPlannerROS: Robot is far away from global plan. distance (" << distance << ") > config.max_follow_distance (" << config.max_follow_distance << ") It probably has crashed.");
                is_crashed = true;
                return FINISHED;
            }

            // check if we're done following
            if (current_index == global_plan.size() - 2)
            {
                ROS_INFO_STREAM("FTCLocalPlannerROS: switching planner to position mode");
                return WAITING_FOR_GOAL_APPROACH;
            }
        }
        break;
        case WAITING_FOR_GOAL_APPROACH:
        {
            double distance = local_control_point.translation().norm();
            if (time_in_current_state() > config.goal_timeout)
            {
                ROS_WARN_STREAM("FTCLocalPlannerROS: Could not reach goal position. config.goal_timeout (" << config.goal_timeout << ") reached - Attempting final rotation anyways.");
                return POST_ROTATE;
            }
            if (distance < config.max_goal_distance_error)
            {
                ROS_INFO_STREAM("FTCLocalPlannerROS: Reached goal position.");
                return POST_ROTATE;
            }
        }
        break;
        case POST_ROTATE:
        {
            if (time_in_current_state() > config.goal_timeout)
            {
                ROS_WARN_STREAM("FTCLocalPlannerROS: Could not reach goal rotation. config.goal_timeout (" << config.goal_timeout << ") reached");
                return FINISHED;
            }
            if (abs(angle_error) * (180.0 / M_PI) < config.max_goal_angle_error)
            {
                ROS_INFO_STREAM("FTCLocalPlannerROS: POST_ROTATE finished.");
                return FINISHED;
            }
        }
        break;
        case FINISHED:
        {
            // Nothing to do here
        }
        break;
        }

        return current_state;
    }

    void FTCPlanner::update_control_point(double dt)
    {

        switch (current_state)
        {
        case PRE_ROTATE:
            tf2::fromMsg(global_plan[0].pose, current_control_point);
            break;
        case FOLLOWING:
        {
            // Normal planner operation
            double straight_dist = distanceLookahead();
            double speed;
            if (straight_dist >= config.speed_fast_threshold)
            {
                speed = config.speed_fast;
            }
            else
            {
                speed = config.speed_slow;
            }

            if (speed > current_movement_speed)
            {
                // accelerate
                current_movement_speed += dt * config.acceleration;
                if (current_movement_speed > speed)
                    current_movement_speed = speed;
            }
            else if (speed < current_movement_speed)
            {
                // decelerate
                current_movement_speed -= dt * config.acceleration;
                if (current_movement_speed < speed)
                    current_movement_speed = speed;
            }

            double distance_to_move = dt * current_movement_speed;
            double angle_to_move = dt * config.speed_angular * (M_PI / 180.0);

            Eigen::Affine3d nextPose, currentPose;
            while (angle_to_move > 0 && distance_to_move > 0 && current_index < global_plan.size() - 2)
            {

                tf2::fromMsg(global_plan[current_index].pose, currentPose);
                tf2::fromMsg(global_plan[current_index + 1].pose, nextPose);

                double pose_distance = (nextPose.translation() - currentPose.translation()).norm();

                Eigen::Quaternion<double> current_rot(currentPose.linear());
                Eigen::Quaternion<double> next_rot(nextPose.linear());

                double pose_distance_angular = current_rot.angularDistance(next_rot);

                if (pose_distance <= 0.0)
                {
                    ROS_WARN_STREAM("FTCLocalPlannerROS: Skipping duplicate point in global plan.");
                    current_index++;
                    continue;
                }

                double remaining_distance_to_next_pose = pose_distance * (1.0 - current_progress);
                double remaining_angular_distance_to_next_pose = pose_distance_angular * (1.0 - current_progress);

                if (remaining_distance_to_next_pose < distance_to_move &&
                    remaining_angular_distance_to_next_pose < angle_to_move)
                {
                    // we need to move further than the remaining distance_to_move. Skip to the next point and decrease distance_to_move.
                    current_progress = 0.0;
                    current_index++;
                    distance_to_move -= remaining_distance_to_next_pose;
                    angle_to_move -= remaining_angular_distance_to_next_pose;
                }
                else
                {
                    // we cannot reach the next point yet, so we update the percentage
                    double current_progress_distance =
                        (pose_distance * current_progress + distance_to_move) / pose_distance;
                    double current_progress_angle =
                        (pose_distance_angular * current_progress + angle_to_move) / pose_distance_angular;
                    current_progress = fmin(current_progress_angle, current_progress_distance);
                    if (current_progress > 1.0)
                    {
                        ROS_WARN_STREAM("FTCLocalPlannerROS: FTC PLANNER: Progress > 1.0");
                        //                    current_progress = 1.0;
                    }
                    distance_to_move = 0;
                    angle_to_move = 0;
                }
            }

            tf2::fromMsg(global_plan[current_index].pose, currentPose);
            tf2::fromMsg(global_plan[current_index + 1].pose, nextPose);
            // interpolate between points
            Eigen::Quaternion<double> rot1(currentPose.linear());
            Eigen::Quaternion<double> rot2(nextPose.linear());

            Eigen::Vector3d trans1 = currentPose.translation();
            Eigen::Vector3d trans2 = nextPose.translation();

            Eigen::Affine3d result;
            result.translation() = (1.0 - current_progress) * trans1 + current_progress * trans2;
            result.linear() = rot1.slerp(current_progress, rot2).toRotationMatrix();

            current_control_point = result;
        }
        break;
        case POST_ROTATE:
            tf2::fromMsg(global_plan[global_plan.size() - 1].pose, current_control_point);
            break;
        case WAITING_FOR_GOAL_APPROACH:
            break;
        case FINISHED:
            break;
        }

        {
            geometry_msgs::PoseStamped viz;
            viz.header = global_plan[current_index].header;
            viz.pose = tf2::toMsg(current_control_point);
            global_point_pub.publish(viz);
        }
        auto map_to_base = tf_buffer->lookupTransform("base_link", "map", ros::Time(), ros::Duration(1.0));
        tf2::doTransform(current_control_point, local_control_point, map_to_base);

        lat_error = local_control_point.translation().y();
        lon_error = local_control_point.translation().x();
        angle_error = local_control_point.rotation().eulerAngles(0, 1, 2).z();
    }

    void FTCPlanner::calculate_velocity_commands(double dt, geometry_msgs::TwistStamped &cmd_vel)
    {
        // check, if we're completely done
        if (current_state == FINISHED || is_crashed)
        {
            cmd_vel.twist.linear.x = 0;
            cmd_vel.twist.angular.z = 0;
            return;
        }

        i_lon_error += lon_error * dt;
        i_lat_error += lat_error * dt;
        i_angle_error += angle_error * dt;

        if (i_lon_error > config.ki_lon_max)
        {
            i_lon_error = config.ki_lon_max;
        }
        else if (i_lon_error < -config.ki_lon_max)
        {
            i_lon_error = -config.ki_lon_max;
        }
        if (i_lat_error > config.ki_lat_max)
        {
            i_lat_error = config.ki_lat_max;
        }
        else if (i_lat_error < -config.ki_lat_max)
        {
            i_lat_error = -config.ki_lat_max;
        }
        if (i_angle_error > config.ki_ang_max)
        {
            i_angle_error = config.ki_ang_max;
        }
        else if (i_angle_error < -config.ki_ang_max)
        {
            i_angle_error = -config.ki_ang_max;
        }

        double d_lat = (lat_error - last_lat_error) / dt;
        double d_lon = (lon_error - last_lon_error) / dt;
        double d_angle = (angle_error - last_angle_error) / dt;

        last_lat_error = lat_error;
        last_lon_error = lon_error;
        last_angle_error = angle_error;

        // allow linear movement only if in following state

        if (current_state == FOLLOWING)
        {
            double lin_speed = lon_error * config.kp_lon + i_lon_error * config.ki_lon + d_lon * config.kd_lon;
            if (lin_speed < 0 && config.forward_only)
            {
                lin_speed = 0;
            }
            else
            {
                if (lin_speed > config.max_cmd_vel_speed)
                {
                    lin_speed = config.max_cmd_vel_speed;
                }
                else if (lin_speed < -config.max_cmd_vel_speed)
                {
                    lin_speed = -config.max_cmd_vel_speed;
                }

                if (lin_speed < 0)
                {
                    lat_error *= -1.0;
                }
            }
            cmd_vel.twist.linear.x = lin_speed;
        }
        else
        {
            cmd_vel.twist.linear.x = 0.0;
        }

        if (current_state == FOLLOWING)
        {

            double ang_speed = angle_error * config.kp_ang + i_angle_error * config.ki_ang + d_angle * config.kd_ang +
                               lat_error * config.kp_lat + i_lat_error * config.ki_lat + d_lat * config.kd_lat;

            if (ang_speed > config.max_cmd_vel_ang)
            {
                ang_speed = config.max_cmd_vel_ang;
            }
            else if (ang_speed < -config.max_cmd_vel_ang)
            {
                ang_speed = -config.max_cmd_vel_ang;
            }

            cmd_vel.twist.angular.z = ang_speed;
        }
        else
        {
            double ang_speed = angle_error * config.kp_ang + i_angle_error * config.ki_ang + d_angle * config.kd_ang;
            if (ang_speed > config.max_cmd_vel_ang)
            {
                ang_speed = config.max_cmd_vel_ang;
            }
            else if (ang_speed < -config.max_cmd_vel_ang)
            {
                ang_speed = -config.max_cmd_vel_ang;
            }

            cmd_vel.twist.angular.z = ang_speed;

            // check if robot oscillates
            bool is_oscillating = checkOscillation(cmd_vel);
            if (is_oscillating)
            {
                ang_speed = config.max_cmd_vel_ang;
                cmd_vel.twist.angular.z = ang_speed;
            }
        }

        if (config.debug_pid)
        {
            ftc_local_planner::PID debugPidMsg;
            debugPidMsg.kp_lon_set = lon_error;

            // proportional
            debugPidMsg.kp_lat_set = lat_error * config.kp_lat;
            debugPidMsg.kp_lon_set = lon_error * config.kp_lon;
            debugPidMsg.kp_ang_set = angle_error * config.kp_ang;

            // integral
            debugPidMsg.ki_lat_set = i_lat_error * config.ki_lat;
            debugPidMsg.ki_lon_set = i_lon_error * config.ki_lon;
            debugPidMsg.ki_ang_set = i_angle_error * config.ki_ang;

            // diff
            debugPidMsg.kd_lat_set = d_lat * config.kd_lat;
            debugPidMsg.kd_lon_set = d_lon * config.kd_lon;
            debugPidMsg.kd_ang_set = d_angle * config.kd_ang;

            // errors
            debugPidMsg.lon_err = lon_error;
            debugPidMsg.lat_err = lat_error;
            debugPidMsg.ang_err = angle_error;

            // speeds
            debugPidMsg.ang_speed = cmd_vel.twist.angular.z;
            debugPidMsg.lin_speed = cmd_vel.twist.linear.x;

            pubPid.publish(debugPidMsg);
        }
    }

    bool FTCPlanner::getProgress(PlannerGetProgressRequest &req, PlannerGetProgressResponse &res)
    {
        res.index = current_index;
        return true;
    }

    bool FTCPlanner::checkCollision(int max_points)
    {
        visualization_msgs::Marker obstacle_marker;

        if (!config.check_obstacles)
        {
            return false;
        }
        // maximal costs
        unsigned char previous_cost = 255;
        // ensure look ahead not out of plan
        if (global_plan.size() < max_points)
        {
            max_points = global_plan.size();
        }
        for (int i = 0; i < max_points; i++)
        {
            geometry_msgs::PoseStamped x_pose;

            x_pose = global_plan[current_index + i];

            unsigned int x;
            unsigned int y;
            if (costmap_map_->worldToMap(x_pose.pose.position.x, x_pose.pose.position.y, x, y))
            {
                unsigned char costs = costmap_map_->getCost(x, y);
                if (config.debug_obstacle)
                {
                    debugObstacle(obstacle_marker, x, y, costs, max_points);
                }
                // Near at obstacel
                if (costs > 0)
                {
                    // Possible collision
                    if (costs > 127 && costs > previous_cost)
                    {
                        ROS_WARN("FTCLocalPlannerROS: Possible collision. Stop local planner.");
                        return true;
                    }
                }
                previous_cost = costs;
            }
        }
        return false;
    }

    bool FTCPlanner::checkOscillation(geometry_msgs::TwistStamped &cmd_vel)
    {
        bool oscillating = false;
        // detect and resolve oscillations
        if (config.oscillation_recovery)
        {
            // oscillating = true;
            double max_vel_theta = config.max_cmd_vel_ang;
            double max_vel_current = config.max_cmd_vel_speed;

            failure_detector_.update(cmd_vel, config.max_cmd_vel_speed, config.max_cmd_vel_speed, max_vel_theta,
                                     config.oscillation_v_eps, config.oscillation_omega_eps);

            oscillating = failure_detector_.isOscillating();

            if (oscillating) // we are currently oscillating
            {
                if (!oscillation_detected_) // do we already know that robot oscillates?
                {
                    time_last_oscillation_ = ros::Time::now(); // save time when oscillation was detected
                    oscillation_detected_ = true;
                }
                // calculate duration of actual oscillation
                bool oscillation_duration_timeout = !((ros::Time::now() - time_last_oscillation_).toSec() < config.oscillation_recovery_min_duration); // check how long we oscillate
                if (oscillation_duration_timeout)
                {
                    if (!oscillation_warning_) // ensure to send warning just once instead of spamming around
                    {
                        ROS_WARN("FTCLocalPlannerROS: possible oscillation (of the robot or its local plan) detected. Activating recovery strategy (prefer current turning direction during optimization).");
                        oscillation_warning_ = true;
                    }
                    return true;
                }
                return false; // oscillating but timeout not reached
            }
            else
            {
                // not oscillating
                time_last_oscillation_ = ros::Time::now(); // save time when oscillation was detected
                oscillation_detected_ = false;
                oscillation_warning_ = false;
                return false;
            }
        }
        return false; // no check for oscillation
    }

    void FTCPlanner::debugObstacle(visualization_msgs::Marker &obstacle_points, double x, double y, unsigned char cost, int maxIDs)
    {
        if (obstacle_points.points.empty())
        {
            obstacle_points.header.frame_id = costmap->getGlobalFrameID();
            obstacle_points.header.stamp = ros::Time::now();
            obstacle_points.action = visualization_msgs::Marker::ADD;
            obstacle_points.pose.orientation.w = 1.0;
            obstacle_points.type = visualization_msgs::Marker::POINTS;
            obstacle_points.scale.x = 0.2;
            obstacle_points.scale.y = 0.2;
        }
        obstacle_points.id = obstacle_points.points.size() + 1;

        if (cost < 127)
        {
            obstacle_points.color.g = 1.0f;
        }

        if (cost >= 127 && cost < 255)
        {
            obstacle_points.color.g = 0.5f;
            obstacle_points.color.r = 0.5f;
        }
        else
        {
            obstacle_points.color.r = 1.0f;
        }
        obstacle_points.color.a = 1.0;
        geometry_msgs::Point p;
        costmap_map_->mapToWorld(x, y, p.x, p.y);
        p.z = 0;

        // debug footprint


        obstacle_points.points.push_back(p);
        if (obstacle_points.points.size() >= maxIDs || cost > 0)
        {
            obstacle_marker_pub.publish(obstacle_points);
            obstacle_points.points.clear();
        }
    }

    bool FTCPlanner::isTrajectoryFeasible(base_local_planner::CostmapModel *costmap_model, const std::vector<geometry_msgs::Point> &footprint_spec,
                                          double inscribed_radius, double circumscribed_radius, int look_ahead_idx) //, double feasibility_check_lookahead_distance)
    {
        visualization_msgs::Marker obstacle_marker;
        // ensure look ahead not out of plan
        if (global_plan.size() < look_ahead_idx)
        {
            look_ahead_idx = global_plan.size();
        }
        for (int i = 0; i <= look_ahead_idx; ++i)
        {
            geometry_msgs::PoseStamped x_pose;
            int index = current_index + i;
            if (index > global_plan.size())
            {
                index = global_plan.size();
            }
            x_pose = global_plan[index];
            unsigned int x;
            unsigned int y;
            if (costmap_map_->worldToMap(x_pose.pose.position.x, x_pose.pose.position.y, x, y))
            {
             
                double costs = costmap_model->footprintCost(x_pose.pose.position, footprint_spec, inscribed_radius, circumscribed_radius);
                if (config.debug_obstacle)
                {

                    if (costs > 0)
                    {
                        debugObstacle(obstacle_marker, x, y, 0, look_ahead_idx);
                    }
                    if (costs == -3) // off map
                    {
                        debugObstacle(obstacle_marker, x, y, 127, look_ahead_idx);
                    }
                    if (costs == -1) // collission
                    {
                        debugObstacle(obstacle_marker, x, y, 255, look_ahead_idx);
                    }                    
                }
                if (costs == -1)
                {
                    ROS_WARN("FTCLocalPlannerROS: Possible collision. Stop local planner.");
                    return false;
                }
                // Checks if the distance between two poses is higher than the robot radius or the orientation diff is bigger than the specified threshold
                // and interpolates in that case.
                // (if obstacles are pushing two consecutive poses away, the center between two consecutive poses might coincide with the obstacle ;-)!
                // if (i < look_ahead_idx)
                // {
                //     double delta_rot = g2o::normalize_theta(g2o::normalize_theta(teb().Pose(i + 1).theta()) -
                //                                             g2o::normalize_theta(teb().Pose(i).theta()));
                //     Eigen::Vector2d delta_dist = teb().Pose(i + 1).position() - teb().Pose(i).position();
                //     if (fabs(delta_rot) > cfg_->trajectory.min_resolution_collision_check_angular || delta_dist.norm() > inscribed_radius)
                //     {
                //         int n_additional_samples = std::max(std::ceil(fabs(delta_rot) / cfg_->trajectory.min_resolution_collision_check_angular),
                //                                             std::ceil(delta_dist.norm() / inscribed_radius)) -
                //                                    1;
                //         PoseSE2 intermediate_pose = teb().Pose(i);
                //         for (int step = 0; step < n_additional_samples; ++step)
                //         {
                //             intermediate_pose.position() = intermediate_pose.position() + delta_dist / (n_additional_samples + 1.0);
                //             intermediate_pose.theta() = g2o::normalize_theta(intermediate_pose.theta() +
                //                                                              delta_rot / (n_additional_samples + 1.0));
                //             if (costmap_model->footprintCost(intermediate_pose.x(), intermediate_pose.y(), intermediate_pose.theta(),
                //                                              footprint_spec, inscribed_radius, circumscribed_radius) == -1)
                //             {
                //                 if (visualization_)
                //                 {
                //                     visualization_->publishInfeasibleRobotPose(intermediate_pose, *robot_model_, footprint_spec);
                //                 }
                //                 return false;
                //             }
                //         }
                //     }
                // }
            }
        }
        return true;
    }

}
