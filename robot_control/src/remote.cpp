#include <rclcpp/rclcpp.hpp>
#include <moveit/move_group_interface/move_group_interface.h>

#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit_visual_tools/moveit_visual_tools.h>


#include <moveit/trajectory_processing/iterative_time_parameterization.h>
#include "sensor_msgs/msg/joint_state.hpp"


#include <tf2/LinearMath/Quaternion.h>

#include <string.h>
#include <thread>
#include <atomic>
#include <mutex>

#include "robot_control_msgs/msg/pose.hpp"
#include "std_msgs/msg/int16.hpp"


using namespace std;
using moveit::planning_interface::MoveGroupInterface;


const double tau = 2 * M_PI;
float scaled_dx, scaled_dy, scaled_dz;
atomic<float> dx;
atomic<float> dy;
atomic<float> dz;
atomic<float> qw;
atomic<float> qx;
atomic<float> qy;
atomic<float> qz;
tf2::Quaternion dq(0,0,0,1);

string ready_pose_name;
int ready_pose_idx;

mutex q_mtx;

atomic<int> mode(-1);

static const std::string PLANNING_GROUP = "ur_manipulator";

moveit::planning_interface::MoveGroupInterface* move_group_interface;
const moveit::core::JointModelGroup* joint_model_group;
moveit_visual_tools::MoveItVisualTools* visual_tools;

rclcpp::Publisher<trajectory_msgs::msg::JointTrajectory>::SharedPtr traj_pub;

float scale = 1;
vector<float> limits;

geometry_msgs::msg::PoseStamped start_pose;
vector<vector<geometry_msgs::msg::Pose>> trajectories;
thread* plan = nullptr;
trajectory_msgs::msg::JointTrajectory plan_traj;




void getControlValue(robot_control_msgs::msg::Pose pose_)
{
  q_mtx.lock();
  dx = dx + pose_.dx * scale;
  dy = dy + pose_.dy * scale;
  dz = dz + pose_.dz * scale;

  //align coordinate of controller and robot here
  scaled_dx = dx;
  scaled_dy = dy;
  scaled_dz = dz;
  
  //control only position, if you need orientation, uncomment lines below (not tested)
  //tf2::Quaternion q(pose_->qx, pose_->qy, pose_->qz, pose_->qw);
  // dq = q * dq;

  q_mtx.unlock();
  cout<<"control value"<<scaled_dx<<" "<<scaled_dy<<" "<<scaled_dz<<endl;
  
}

void motionPlan()
{
  while(true)
  {
    this_thread::sleep_for(chrono::milliseconds(100));
    if(fabs(scaled_dx) < 1e-4 && fabs(scaled_dy) < 1e-4 && fabs(scaled_dz) < 1e-4)
    {
      continue;
    }

    
    //get target pose
    geometry_msgs::msg::Pose target_pose;
    auto target_pose_now = move_group_interface -> getCurrentPose();
    target_pose = target_pose_now.pose;
    q_mtx.lock();
    target_pose.position.x += scaled_dx;
    target_pose.position.y += scaled_dy;
    target_pose.position.z += scaled_dz;
    q_mtx.unlock();
    cout<<"goal pos "<<target_pose.position.x<<" "<<target_pose.position.y<<" "<<target_pose.position.z<<endl;


    //use setFromIK rather than move_group.plan, which give bad ik solution
    //when using move_group.plan, a small change in position cause a large change in joint angle
    //not sure it is a bug or something
    moveit::core::RobotStatePtr current_state = move_group_interface -> getCurrentState(10);
    joint_model_group = move_group_interface -> getCurrentState()->getJointModelGroup(PLANNING_GROUP);

    std::vector<double> start_joint_positions;
    current_state->copyJointGroupPositions(joint_model_group, start_joint_positions);
    current_state->setFromIK(joint_model_group, target_pose, 0.0);

    std::vector<double> solution;
    current_state->copyJointGroupPositions(joint_model_group, solution);
    move_group_interface -> setJointValueTarget(solution);

    moveit::planning_interface::MoveGroupInterface::Plan trajectory;
    bool success = (move_group_interface -> plan(trajectory) == moveit::planning_interface::MoveItErrorCode::SUCCESS);
    
    if(success)
    {
      plan_traj = trajectory.trajectory_.joint_trajectory;
      traj_pub -> publish(plan_traj);

    }
    
    scaled_dx = 0;
    scaled_dy = 0;
    scaled_dz = 0;
    dx = 0;
    dy = 0;
    dz = 0;
  }
}


int main(int argc, char** argv)
{
  
  rclcpp::init(argc, argv);
  rclcpp::NodeOptions node_options;
  node_options.automatically_declare_parameters_from_overrides(true);
  auto node = rclcpp::Node::make_shared("robot_control", node_options);

  //subscribe pose infomation
  rclcpp::Subscription<robot_control_msgs::msg::Pose>::SharedPtr pose_sub;
  pose_sub = node -> create_subscription<robot_control_msgs::msg::Pose>("pose",10,&getControlValue);


  traj_pub = node -> create_publisher<trajectory_msgs::msg::JointTrajectory>("joint_trajectory",10);

  //use a MultiThreadedExecutor, since we need to get robot info from other node
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  std::thread([&executor]() { executor.spin(); }).detach();
  

  move_group_interface = new moveit::planning_interface::MoveGroupInterface(node, PLANNING_GROUP);

  //
  //you can add visual and collision settings here
  //

  move_group_interface -> setMaxAccelerationScalingFactor(0.1);
  move_group_interface -> setMaxVelocityScalingFactor(0.1);

  move_group_interface -> setGoalPositionTolerance(0.0005);
  move_group_interface -> setGoalOrientationTolerance(0.001);


  plan = new thread(&motionPlan);
  plan -> join();

  return 0;
}