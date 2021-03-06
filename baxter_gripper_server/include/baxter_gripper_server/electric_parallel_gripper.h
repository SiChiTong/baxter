/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, University of Colorado, Boulder
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman
   Desc:   Models an electric parallel gripper
*/

// ROS
#include <ros/ros.h>
#include <actionlib/server/simple_action_server.h>

// Messages
#include <std_msgs/Float32.h>
#include <std_msgs/Empty.h>
#include <std_msgs/Bool.h>
#include <baxter_msgs/GripperState.h>
#include <baxter_msgs/DigitalIOState.h>
#include <sensor_msgs/JointState.h>
#include <control_msgs/GripperCommandAction.h>

namespace baxter_gripper_server
{

static const std::string GRIPPER_COMMAND_ACTION_TOPIC="baxter_gripper_action";
static const std::string BASE_LINK = "base"; //"/base";

// Copied from URDF \todo read straight from URDF?
static const double FINGER_JOINT_UPPER = 0.0095; //open
static const double FINGER_JOINT_LOWER = -0.0125; //close

// Various numbers
static const double MSG_PULSE_SEC = 0.1;
static const double WAIT_GRIPPER_CLOSE_SEC = 0.5;
static const double WAIT_STATE_MSG_SEC = 1; // max time to wait for the gripper state to refresh
static const double GRIPPER_MSG_RESEND = 2; // Number of times to re-send a msg to the end effects for assurance that it arrives

class ElectricParallelGripper
{
protected:

  // A shared node handle
  ros::NodeHandle nh_;

  // Action Server
  actionlib::SimpleActionServer<control_msgs::GripperCommandAction> action_server_;

  // Publishers
  ros::Publisher calibrate_topic_;
  ros::Publisher position_topic_;
  ros::Publisher release_topic_;
  ros::Publisher reset_topic_;
  ros::Publisher joint_state_topic_;

  // Subscribers
  ros::Subscriber gripper_state_sub_;
  ros::Subscriber cuff_grasp_sub_;
  ros::Subscriber cuff_ok_sub_;

  // Action messages
  control_msgs::GripperCommandResult action_result_;

  // Cache an empty message
  std_msgs::Empty empty_msg_;
  std_msgs::Bool bool_msg_;
  std_msgs::Float32 zero_msg_;

  // Remember the last gripper state and time
  baxter_msgs::GripperStateConstPtr gripper_state_;
  ros::Time gripper_state_timestamp_;

  enum gripper_error_msgs {NO_ERROR, EXPIRED, CALIBRATED, ENABLED, ERROR, READY};

  // Button states
  bool cuff_grasp_pressed_;
  bool cuff_ok_pressed_;

  bool in_simulation_; // Using Gazebo or not
  double finger_joint_stroke_; // cache the diff between upper and lower limits
  double finger_joint_midpoint_; // cache the mid point of the joint limits
  std::string arm_name_; // Remember which arm this class is for

public:

  // Constructor
  ElectricParallelGripper(const std::string action_name, const std::string arm_name, const bool in_simulation)
    : action_server_(nh_, action_name, false),
      arm_name_(arm_name),
      in_simulation_(in_simulation),
      cuff_grasp_pressed_(false),
      cuff_ok_pressed_(false)
  {
    ROS_DEBUG_STREAM_NAMED(arm_name_, "Baxter Electric Parallel Gripper starting " << arm_name_);

    // Start the publishers
    calibrate_topic_ = nh_.advertise<std_msgs::Empty>("/robot/limb/" + arm_name_
                       + "/accessory/gripper/command_calibrate",10);

    position_topic_ = nh_.advertise<std_msgs::Float32>("/robot/limb/" + arm_name_
                      + "/accessory/gripper/command_grip",10);

    release_topic_ = nh_.advertise<std_msgs::Empty>("/robot/limb/" + arm_name_
                     + "/accessory/gripper/command_release",10);

    reset_topic_ = nh_.advertise<std_msgs::Bool>("/robot/limb/" + arm_name_
                   + "/accessory/gripper/command_reset",10);

    // Start the subscribers
    gripper_state_sub_ = nh_.subscribe<baxter_msgs::GripperState>("/sdk/robot/limb/" + arm_name_
                         + "/accessory/gripper/state",
                         1, &ElectricParallelGripper::stateCallback, this);

    cuff_grasp_sub_ = nh_.subscribe<baxter_msgs::DigitalIOState>("/sdk/robot/digital_io/" +
                      arm_name_ + "_upper_button/state",
                      1, &ElectricParallelGripper::cuffGraspCallback, this);

    cuff_ok_sub_ = nh_.subscribe<baxter_msgs::DigitalIOState>("/sdk/robot/digital_io/" +
                   arm_name_ + "_lower_button/state",
                   1, &ElectricParallelGripper::cuffOKCallback, this);

    // Decide if we are in simulation based on the existence of the gripper state message
    if( !in_simulation )
    {
      int count = 0;
      while( ros::ok() && gripper_state_timestamp_.toSec() == 0 )
      {
        if( count > 40 ) // 20 is an arbitrary number for when to assume we are in simulation mode
        {
          ROS_INFO_STREAM_NAMED(arm_name_,"Assuming Baxter is in simulation mode because unable to get gripper state");
          in_simulation_ = true;
          break;
        }

        ++count;
        ros::Duration(0.05).sleep();
      }
    }

    // If in simulation, fill in dummy state values
    if( in_simulation_ )
    {
      baxter_msgs::GripperStatePtr simulation_state;
      simulation_state.reset(new baxter_msgs::GripperState());
      simulation_state->enabled = 1;
      simulation_state->calibrated = 1;
      simulation_state->ready = 1;
      simulation_state->moving = 0;
      simulation_state->gripping = 0;
      simulation_state->missed = 0;
      simulation_state->error = 0;
      simulation_state->command = 0; // \todo
      simulation_state->position = 0; // \todo
      simulation_state->force = 7; // base line value unloaded
      gripper_state_ = simulation_state;
    }

    // Gazebo publishes a joint state for the gripper, but Baxter does not do so in the right format
    if( !in_simulation_ )
    {
      joint_state_topic_ = nh_.advertise<sensor_msgs::JointState>("/robot/joint_states",10);
    }

    // Cache zero command
    zero_msg_.data = 0;

    // Calculate joint stroke and midpoint
    finger_joint_stroke_ = FINGER_JOINT_UPPER - FINGER_JOINT_LOWER;
    finger_joint_midpoint_ = FINGER_JOINT_LOWER + finger_joint_stroke_ / 2;

    // Get the EE ready to be used - calibrate if needed
    autoFix(false);

    // Error report
    if( hasError() )
    {
      ROS_ERROR_STREAM_NAMED(arm_name,"Unable to enable " << arm_name_ << " gripper, perhaps the EStop is on. Quitting.");
      exit(0);
    }
    else
    {
      // Register the goal and start
      action_server_.registerGoalCallback(boost::bind(&ElectricParallelGripper::goalCallback, this));

      action_server_.start();

      // Announce state
      ROS_INFO_STREAM_NAMED(arm_name_, "Baxter Electric Parallel Gripper ready " << arm_name_);
    }
  }

  bool autoFix(bool verbose = true)
  {
    int attempts = 0;
    bool result = false;

    while(ros::ok())
    {
      bool recheck = false;

      switch(checkError())
      {
        case EXPIRED:
          recheck = true;
          break;

        case ENABLED:
          recheck = true;
          break;

        case ERROR:
          resetError();
          if( gripper_state_->error )
          {
            recheck = true;
          }
          break;

        case CALIBRATED:
          calibrate();
          if( !gripper_state_->calibrated )
          {
            recheck = true;
          }
          break;

        case READY:
          recheck = true;
          break;

        case NO_ERROR:
        default:
          break;
          // do nothing
      }

      // Check if we need to loop again
      if(!recheck)
      {
        result = true;
        break;
      }

      // Check if we should give up
      if( attempts > 2 )
      {
        result = false;
        break;
      }

      //if(verbose)
      //  ROS_DEBUG_STREAM_NAMED(arm_name_,"Autofix detected issue with end effector " << arm_name_ << ". Attempting to fix. State: \n" << *gripper_state_);

      ros::Duration(WAIT_STATE_MSG_SEC).sleep();
      ros::spinOnce();
      ++attempts;
    }

    return result;
  }

  void populateState(sensor_msgs::JointState &state)
  {
    state.header.frame_id = BASE_LINK;
    state.header.stamp = gripper_state_timestamp_;
    state.name.push_back(arm_name_ + "_gripper_l_finger_joint");
    state.name.push_back(arm_name_ + "_gripper_r_finger_joint"); // \todo remove this mimic joint once moveit is fixed
    state.velocity.push_back(0);
    state.velocity.push_back(0); // \todo remove this mimic joint once moveit is fixed
    state.effort.push_back(gripper_state_->force);
    state.effort.push_back(gripper_state_->force); // \todo remove this mimic joint once moveit is fixed

    // Convert 0-100 state to joint position
    double position = FINGER_JOINT_LOWER + finger_joint_stroke_ *
      (gripper_state_->position / 100);

    state.position.push_back(position);
    state.position.push_back(position*-1); // \todo remove this mimic joint once moveit is fixed
  }

  bool isInSimulation()
  {
    return in_simulation_;
  }

  /**
   * \brief Send the calibrate command to the EE twice (just in case one fails)
   but do not do any error checking
  */
  void calibrate()
  {
    if( in_simulation_ )
      return;

    // Calibrate if needed
    if( !gripper_state_->calibrated )
    {
      ROS_INFO_STREAM_NAMED(arm_name_,"Calibrating gripper");

      calibrate_topic_.publish(empty_msg_);
      ros::spinOnce();
      ros::Duration(0.05).sleep();

      calibrate_topic_.publish(empty_msg_);
      ros::spinOnce();
      ros::Duration(2.0).sleep();
    }
  }

  void stateCallback(const baxter_msgs::GripperStateConstPtr& msg)
  {
    gripper_state_ = msg;
    gripper_state_timestamp_ = ros::Time::now();

    // Check for errors every 50 refreshes
    static std::size_t counter = 0;
    counter++;
    if( counter % 10 == 0 )
    {
      if( !autoFix() )
      {
        ROS_ERROR_STREAM_THROTTLE_NAMED(2,arm_name_,"End effector " << arm_name_ << " in error state:\n" << *gripper_state_);
      }
      counter = 0; // Reset counter
    }
  }

  void cuffGraspCallback(const baxter_msgs::DigitalIOStateConstPtr& msg)
  {
    // Check if button is pressed
    if( msg->state == 1 )
    {
      // Check that the button was not already pressed
      if( cuff_grasp_pressed_ == false)
      {
        cuff_grasp_pressed_ = true; // set this first since closeGripper has delay
        closeGripper();
      }
    }
    else
    {
      // Reset so button can be pressed anytime
      cuff_grasp_pressed_ = false;
    }
  }

  void cuffOKCallback(const baxter_msgs::DigitalIOStateConstPtr& msg)
  {
    // Check if button is pressed
    if( msg->state == 1 )
    {
      // Check that the button was not already pressed
      if( !cuff_ok_pressed_ )
      {
        cuff_ok_pressed_ = true;
        openGripper();
      }
    }
    else
    {
      // Reset so button can be pressed anytime
      cuff_ok_pressed_ = false;
    }
  }

  /**
   * \brief Check if gripper is in good state
   * \return error type if there is one
   */
  gripper_error_msgs checkError()
  {
    // Run Checks
    if( !in_simulation_ &&
      ros::Time::now() > gripper_state_timestamp_ + ros::Duration(2.0)) // check that the message timestamp is no older than 1 second
      return EXPIRED;
    if( !gripper_state_->enabled )
      return ENABLED;
    if( gripper_state_->error )
      return ERROR;
    if( !gripper_state_->calibrated )
      return CALIBRATED;
    if( !isReadyMovingGrippingOK() )
      return READY;

    return NO_ERROR;
  }

  /**
   * \brief Check if gripper is in good state
   * \return true if there is no error
   */
  bool hasError()
  {
    // Populate these now in case an error is detected below
    action_result_.position = gripper_state_->position;
    action_result_.effort = gripper_state_->force;
    action_result_.stalled = false; // \todo implement
    action_result_.reached_goal = false;

    switch(checkError())
    {
      case EXPIRED:
        ROS_ERROR_STREAM_NAMED(arm_name_,"Gripper " << arm_name_ << " state expired. State: \n" << *gripper_state_ );

        if( action_server_.isActive() )
          action_server_.setAborted(action_result_,std::string("Gripper state expired"));

        return true;

      case ENABLED:
        ROS_ERROR_STREAM_NAMED(arm_name_,"Gripper " << arm_name_ << " not enabled. State: \n" << *gripper_state_ );

        if( action_server_.isActive() )
          action_server_.setAborted(action_result_,"Gripper not enabled");

        return true;

      case ERROR:
        ROS_ERROR_STREAM_NAMED(arm_name_,"Gripper " << arm_name_ << " has error. State: \n" << *gripper_state_ );

        if( action_server_.isActive() )
          action_server_.setAborted(action_result_,"Gripper has error");

        return true;

      case CALIBRATED:
        ROS_ERROR_STREAM_NAMED(arm_name_,"Gripper " << arm_name_ << " not calibrated. State: \n" << *gripper_state_ );

        if( action_server_.isActive() )
          action_server_.setAborted(action_result_,"Gripper not calibrated");

        return true;

      case READY:
        ROS_ERROR_STREAM_NAMED(arm_name_,"Gripper " << arm_name_ << " not ready. State: \n" << *gripper_state_ );

        if( action_server_.isActive() )
          action_server_.setAborted(action_result_,"Gripper not ready");
        return true;

      case NO_ERROR:
      default:
        // do nothing
        break;
    }

    return false;
  }

  /**
   * \brief Check that the either the ready/moving/gripper status is true
   * \return true if one of the three are enabled
   */
  bool isReadyMovingGrippingOK()
  {
    // Sometimes the state is temporarily between ready, gripping, and moving.
    // If we wait for a second it usually fixes itself:
    int counter = 0;
    while( !gripper_state_->ready && !gripper_state_->gripping && !gripper_state_->moving && ros::ok() )
    {
      ros::Duration(MSG_PULSE_SEC).sleep();
      ros::spinOnce();
      if( counter > WAIT_STATE_MSG_SEC / MSG_PULSE_SEC)
      {
        return false;
      }
      counter++;
    }
    return true;
  }

  /**
   * \brief Send the reset command to the EE twice (just to be sure). No error checking.
   */
  void resetError()
  {
    ROS_INFO_STREAM_NAMED(arm_name_,"Resetting gripper");

    bool_msg_.data = true;
    reset_topic_.publish(bool_msg_);
    ros::Duration(0.05).sleep();
    reset_topic_.publish(bool_msg_);
    ros::Duration(0.05).sleep();
  }

  // Action server sends goals here
  void goalCallback()
  {
    double position = action_server_.acceptNewGoal()->command.position;
    bool success;

    //ROS_INFO_STREAM_NAMED(arm_name_,"Recieved goal for command position: " << position);

    // Open command
    if(position > finger_joint_midpoint_)
    {
      // Error check gripper
      if( hasError() )
        success = false;
      else
        success = openGripper();
    }
    else // Close command
    {
      // Error check gripper
      if( hasError() )
        success = false;
      else
        success = closeGripper();
    }

    // Report success
    action_result_.position = gripper_state_->position;
    action_result_.effort = gripper_state_->force;

    if( success )
    {
      action_result_.reached_goal = true;
      action_server_.setSucceeded(action_result_,"Success");
      action_result_.stalled = false; // \todo implement
    }
    else
    {
      ROS_WARN_STREAM_NAMED(arm_name_,"Failed to complete end effector command");
      action_result_.reached_goal = false;
      action_result_.stalled = true; // \todo is this always true?
      action_server_.setSucceeded(action_result_,"Failure"); // \todo is this succeeded?
    }
  }

  bool openGripper()
  {
    ROS_INFO_STREAM_NAMED(arm_name_,"Opening " << arm_name_ << " end effector");

    // Send command several times to be safe
    for (std::size_t i = 0; i < GRIPPER_MSG_RESEND; ++i)
    {
      release_topic_.publish(empty_msg_);
      ros::Duration(MSG_PULSE_SEC).sleep();
      ros::spinOnce();
    }

    // Error check gripper
    if( hasError() )
      return false;

    return true;
  }

  bool closeGripper()
  {
    ROS_INFO_STREAM_NAMED(arm_name_,"Closing " << arm_name_ << " end effector");

    // Send command several times to be safe
    for (std::size_t i = 0; i < GRIPPER_MSG_RESEND; ++i)
    {
      position_topic_.publish(zero_msg_);
      ros::Duration(MSG_PULSE_SEC).sleep();
      ros::spinOnce();
    }

    // Check that it actually grasped something
    int counter = 0;
    while( !gripper_state_->gripping )
    {
      ros::Duration(MSG_PULSE_SEC).sleep();
      ros::spinOnce();

      if( counter > WAIT_GRIPPER_CLOSE_SEC / MSG_PULSE_SEC )
        break;
      counter++;
    }
    if( !gripper_state_->gripping && !in_simulation_)
    {
      ROS_WARN_STREAM_NAMED(arm_name_,"No object detected in end effector");
      return false;
    }

    // Error check gripper
    if( hasError() )
      return false;

    return true;
  }

}; // end of class

} // namespace
