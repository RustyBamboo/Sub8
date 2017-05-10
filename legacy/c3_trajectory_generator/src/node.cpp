#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseStamped.h>
#include <actionlib/server/simple_action_server.h>

#include <mil_msgs/PoseTwistStamped.h>
#include <mil_tools/msg_helpers.hpp>
#include <mil_tools/param_helpers.hpp>
#include <ros_alarms/listener.hpp>

#include <mil_msgs/MoveToAction.h>
#include "c3_trajectory_generator/SetDisabled.h"
#include "C3Trajectory.h"

using namespace std;
using namespace geometry_msgs;
using namespace nav_msgs;
using namespace mil_msgs;
using namespace mil_tools;
using namespace c3_trajectory_generator;

subjugator::C3Trajectory::Point Point_from_PoseTwist(const Pose &pose, const Twist &twist) {
  tf::Quaternion q;
  tf::quaternionMsgToTF(pose.orientation, q);

  subjugator::C3Trajectory::Point res;

  res.q.head(3) = xyz2vec(pose.position);
  tf::Matrix3x3(q).getRPY(res.q[3], res.q[4], res.q[5]);

  res.qdot.head(3) = vec2vec(tf::Matrix3x3(q) * vec2vec(xyz2vec(twist.linear)));
  res.qdot.tail(3) = (Eigen::Matrix3d() << 1, sin(res.q[3]) * tan(res.q[4]),
                      cos(res.q[3]) * tan(res.q[4]), 0, cos(res.q[3]), -sin(res.q[3]), 0,
                      sin(res.q[3]) / cos(res.q[4]), cos(res.q[3]) / cos(res.q[4])).finished() *
                     xyz2vec(twist.angular);

  return res;
}

PoseTwist PoseTwist_from_PointWithAcceleration(
    const subjugator::C3Trajectory::PointWithAcceleration &p) {
  tf::Quaternion orient = tf::createQuaternionFromRPY(p.q[3], p.q[4], p.q[5]);

  PoseTwist res;

  res.pose.position = vec2xyz<Point>(p.q.head(3));
  quaternionTFToMsg(orient, res.pose.orientation);

  Eigen::Matrix3d worldangvel_from_eulerrates =
      (Eigen::Matrix3d() << 1, 0, -sin(p.q[4]), 0, cos(p.q[3]), sin(p.q[3]) * cos(p.q[4]), 0,
       -sin(p.q[3]), cos(p.q[3]) * cos(p.q[4])).finished();

  res.twist.linear = vec2xyz<Vector3>(tf::Matrix3x3(orient.inverse()) * vec2vec(p.qdot.head(3)));
  res.twist.angular = vec2xyz<Vector3>(worldangvel_from_eulerrates * p.qdot.tail(3));

  res.acceleration.linear =
      vec2xyz<Vector3>(tf::Matrix3x3(orient.inverse()) * vec2vec(p.qdotdot.head(3)));
  res.acceleration.angular = vec2xyz<Vector3>(worldangvel_from_eulerrates * p.qdotdot.tail(3));

  return res;
}

Pose Pose_from_Waypoint(const subjugator::C3Trajectory::Waypoint &wp) {
  Pose res;

  res.position = vec2xyz<Point>(wp.r.q.head(3));
  quaternionTFToMsg(tf::createQuaternionFromRPY(wp.r.q[3], wp.r.q[4], wp.r.q[5]), res.orientation);

  return res;
}

struct Node {
  ros::NodeHandle nh;
  ros::NodeHandle private_nh;
  tf::TransformListener tf_listener;
  ros_alarms::AlarmListener<> kill_listener;

  string body_frame;
  string fixed_frame;
  subjugator::C3Trajectory::Limits limits;
  ros::Duration traj_dt;

  ros::Subscriber odom_sub;
  actionlib::SimpleActionServer<mil_msgs::MoveToAction> actionserver;
  ros::Publisher trajectory_pub;
  ros::Publisher trajectory_vis_pub;
  ros::Publisher waypoint_pose_pub;
  ros::ServiceServer set_disabled_service;

  ros::Timer update_timer;

  bool disabled;
  boost::scoped_ptr<subjugator::C3Trajectory> c3trajectory;
  ros::Time c3trajectory_t;

  subjugator::C3Trajectory::Waypoint current_waypoint;
  ros::Time current_waypoint_t;

  double linear_tolerance, angular_tolerance;

  bool set_disabled(SetDisabledRequest &request, SetDisabledResponse &response) {
    disabled = request.disabled;
    if (disabled) {
      c3trajectory.reset();
    }
    return true;
  }

  Node()
  : private_nh("~"),
    actionserver(nh, "moveto", false),
    disabled(false),
    kill_listener(nh, "kill")
  {
    // Callback to reset trajectory when (un)killing
    auto reset_traj = [this](ros_alarms::AlarmProxy a) { this->c3trajectory.reset(); };
    kill_listener.addRaiseCb(reset_traj);

    // Make sure alarm integration is ok
    kill_listener.waitForConnection(ros::Duration(2));
    if(kill_listener.getNumConnections() < 1)
      throw std::runtime_error("The kill listener isn't connected to the alarm server");
    kill_listener.start();  // Fuck.

    fixed_frame = mil_tools::getParam<std::string>(private_nh, "fixed_frame");
    body_frame = mil_tools::getParam<std::string>(private_nh, "body_frame");

    limits.vmin_b = mil_tools::getParam<subjugator::Vector6d>(private_nh, "vmin_b");
    limits.vmax_b = mil_tools::getParam<subjugator::Vector6d>(private_nh, "vmax_b");
    limits.amin_b = mil_tools::getParam<subjugator::Vector6d>(private_nh, "amin_b");
    limits.amax_b = mil_tools::getParam<subjugator::Vector6d>(private_nh, "amax_b");
    limits.arevoffset_b = mil_tools::getParam<Eigen::Vector3d>(private_nh, "arevoffset_b");
    limits.umax_b = mil_tools::getParam<subjugator::Vector6d>(private_nh, "umax_b");
    traj_dt = mil_tools::getParam<ros::Duration>(private_nh, "traj_dt", ros::Duration(0.0001));

    odom_sub = nh.subscribe<Odometry>("odom", 1, boost::bind(&Node::odom_callback, this, _1));

    trajectory_pub = nh.advertise<PoseTwistStamped>("trajectory", 1);
    trajectory_vis_pub = private_nh.advertise<PoseStamped>("trajectory_v", 1);
    waypoint_pose_pub = private_nh.advertise<PoseStamped>("waypoint", 1);

    update_timer =
        nh.createTimer(ros::Duration(1. / 50), boost::bind(&Node::timer_callback, this, _1));

    actionserver.start();

    set_disabled_service = private_nh.advertiseService<SetDisabledRequest, SetDisabledResponse>(
        "set_disabled", boost::bind(&Node::set_disabled, this, _1, _2));
  }

  void odom_callback(const OdometryConstPtr &odom) {
    if (c3trajectory) return;                            // already initialized
    if (kill_listener.isRaised() || disabled) return;  // only initialize when unkilled
    
    ros::Time now = ros::Time::now();

    subjugator::C3Trajectory::Point current =
        Point_from_PoseTwist(odom->pose.pose, odom->twist.twist);
    current.q[3] = current.q[4] = 0;              // zero roll and pitch
    current.qdot = subjugator::Vector6d::Zero();  // zero velocities

    c3trajectory.reset(new subjugator::C3Trajectory(current, limits));
    c3trajectory_t = now;

    current_waypoint = current;
    current_waypoint_t = now;
  }

  void timer_callback(const ros::TimerEvent &) {
    if (!c3trajectory) return;

    ros::Time now = ros::Time::now();

    if (actionserver.isNewGoalAvailable()) {
      boost::shared_ptr<const mil_msgs::MoveToGoal> goal = actionserver.acceptNewGoal();
      current_waypoint = subjugator::C3Trajectory::Waypoint(
          Point_from_PoseTwist(goal->posetwist.pose, goal->posetwist.twist), goal->speed,
          !goal->uncoordinated);
      current_waypoint_t = now;
      this->linear_tolerance = goal->linear_tolerance;
      this->angular_tolerance = goal->angular_tolerance;
    }
    if (actionserver.isPreemptRequested()) {
      current_waypoint = c3trajectory->getCurrentPoint();
      current_waypoint.r.qdot = subjugator::Vector6d::Zero();  // zero velocities
      current_waypoint_t = now;

      // don't try to make output c3 continuous when cancelled - instead stop as quickly as possible
      c3trajectory.reset(new subjugator::C3Trajectory(current_waypoint.r, limits));
      c3trajectory_t = now;
    }

    while (c3trajectory_t + traj_dt < now) {
      c3trajectory->update(traj_dt.toSec(), current_waypoint,
                           (c3trajectory_t - current_waypoint_t).toSec());
      c3trajectory_t += traj_dt;
    }
    nh.serviceClient<movement_validity::isWaypointValid>("isWaypointValid");
    movement_validity::isWaypointValid srv;
    srv.request = (current_waypoint);    
    if (client.call(srv))
    {
      ROS_INFO("resp: %1d", srv.response.resp);
    }
    else
    {
      ROS_ERROR("Failed to call service isWaypointValid");
    }
    if(srv.response.resp == false)
    {
      current_waypoint.r.qdot = subjugator::Vector6d::Zero();  // zero velocities
      current_waypoint_t = now;

      c3trajectory.reset(new subjugator::C3Trajectory(current_waypoint.r, limits));
      c3trajectory_t = now;
    }

    PoseTwistStamped msg;
    msg.header.stamp = c3trajectory_t;
    msg.header.frame_id = fixed_frame;
    msg.posetwist = PoseTwist_from_PointWithAcceleration(c3trajectory->getCurrentPoint());
    trajectory_pub.publish(msg);

    PoseStamped msgVis;
    msgVis.header = msg.header;
    msgVis.pose = msg.posetwist.pose;
    trajectory_vis_pub.publish(msgVis);

    PoseStamped posemsg;
    posemsg.header.stamp = c3trajectory_t;
    posemsg.header.frame_id = fixed_frame;
    posemsg.pose = Pose_from_Waypoint(current_waypoint);
    waypoint_pose_pub.publish(posemsg);

    if (actionserver.isActive() &&
        c3trajectory->getCurrentPoint().is_approximately(
            current_waypoint.r, max(1e-3, linear_tolerance), max(1e-3, angular_tolerance)) &&
        current_waypoint.r.qdot == subjugator::Vector6d::Zero()) {
      actionserver.setSucceeded();
    }
  }
};

int main(int argc, char **argv) {
  ros::init(argc, argv, "c3_trajectory_generator");

  Node n;

  ros::spin();

  return 0;
}
