//This node is an example for working with the Nav2 to stack to command
//the Unitree Go1 to a certain pose in the map.

#include <exception>
#include <vector>
#include <string>
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_srvs/srv/empty.hpp"
#include "geometry_msgs/msg/quaternion.hpp"
#include "tf2/LinearMath/Quaternion.h"
#include "tf2/LinearMath/Matrix3x3.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.hpp"
#include "nav2_msgs/action/navigate_to_pose.hpp"
#include "unitree_nav_interfaces/srv/nav_to_pose.hpp"
#include "std_msgs/msg/string.hpp"


//https://stackoverflow.com/questions/11714325/how-to-get-enum-item-name-from-its-value
#define STATES \
X(IDLE, "IDLE") \
X(SEND_GOAL, "SEND_GOAL") \
X(WAIT_FOR_GOAL_RESPONSE, "WAIT_FOR_GOAL_RESPONSE") \
X(WAIT_FOR_MOVEMENT_COMPLETE, "WAIT_FOR_MOVEMENT_COMPLETE")

#define X(state, name) state,
enum class State : size_t {STATES};
#undef X

#define X(state, name) name,
std::vector<std::string> STATE_NAMES = {STATES};
#undef X

//https://stackoverflow.com/questions/11421432/how-can-i-output-the-value-of-an-enum-class-in-c11
template <typename Enumeration>
auto to_value(Enumeration const value)
  -> typename std::underlying_type<Enumeration>::type
{
  return static_cast<typename std::underlying_type<Enumeration>::type>(value);
}

auto get_state_name(State state) {
  return STATE_NAMES[to_value(state)];
}

struct Pose2D {
  double x = 0.0;
  double y = 0.0;
  double theta = 0.0;
};

std::tuple<double, double, double> quaternion_to_rpy(const geometry_msgs::msg::Quaternion & q);
geometry_msgs::msg::Quaternion rpy_to_quaternion(double roll, double pitch, double yaw);

using namespace std::chrono_literals;

class guide_dog : public rclcpp::Node
{
public:
  guide_dog()
  : Node("guide_dog")
  {

    //Parameters
    auto param = rcl_interfaces::msg::ParameterDescriptor{};
    param.description = "The frame in which poses are sent.";
    declare_parameter("pose_frame", "map", param);
    goal_msg_.pose.header.frame_id = get_parameter("pose_frame").get_parameter_value().get<std::string>();

    //Timers
    timer_ = create_wall_timer(
      static_cast<std::chrono::milliseconds>(static_cast<int>(interval_ * 1000.0)), 
      std::bind(&guide_dog::timer_callback, this)
    );

    // Subscribers
    voice_command_subscriber_ = create_subscription<std_msgs::msg::String>(
      "voice_command", 10, std::bind(
        &guide_dog::voice_command_callback,
        this, std::placeholders::_1));

    //Services
    srv_nav_to_pose_ = create_service<unitree_nav_interfaces::srv::NavToPose>(
      "unitree_nav_to_pose",
      std::bind(&guide_dog::srv_nav_to_pose_callback, this,
                std::placeholders::_1, std::placeholders::_2)
    );

    srv_cancel_nav_ = create_service<std_srvs::srv::Empty>(
      "unitree_cancel_nav",
      std::bind(&guide_dog::cancel_nav_callback, this,
                std::placeholders::_1, std::placeholders::_2)
    );

    //Service Clients
    // cancel_goal_client_ = create_client<unitree_nav_interfaces::srv::SetBodyRPY>("set_body_rpy");

    //Action Clients
    act_nav_to_pose_ = rclcpp_action::create_client<nav2_msgs::action::NavigateToPose>(
      this,
      "navigate_to_pose"
    );

    RCLCPP_INFO_STREAM(get_logger(), "nav_to_pose node started");
  }

private:
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Service<unitree_nav_interfaces::srv::NavToPose>::SharedPtr srv_nav_to_pose_;
  rclcpp::Service<std_srvs::srv::Empty>::SharedPtr srv_cancel_nav_;
  rclcpp_action::Client<nav2_msgs::action::NavigateToPose>::SharedPtr act_nav_to_pose_;

  double rate_ = 100.0; //Hz
  double interval_ = 1.0 / rate_; //seconds
  State state_ = State::IDLE;
  State state_last_ = state_;
  State state_next_ = state_;
  Pose2D goal_pose_;
  rclcpp_action::Client<nav2_msgs::action::NavigateToPose>::Goal goal_msg_ {};
  bool goal_response_received_ = false;
  rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::SharedPtr goal_handle_ {};
  std::shared_ptr<const nav2_msgs::action::NavigateToPose::Feedback> feedback_ = nullptr;
  std::shared_ptr<rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::WrappedResult>
    result_ = nullptr;
  int one_ = 0;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr voice_command_subscriber_;


  void timer_callback()
  {
    
    state_ = state_next_;

    auto new_state = state_ != state_last_;

    if (new_state) {
      RCLCPP_INFO_STREAM(get_logger(), "nav_to_pose state changed to " << get_state_name(state_));

      state_last_ = state_;
    }

    switch(state_) {
      case State::IDLE:
      {
        break;
      }
      case State::SEND_GOAL:
      {
        if(act_nav_to_pose_->wait_for_action_server(5s)) {
          //Reset status flags and pointers
          goal_response_received_ = false;
          goal_handle_ = rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::SharedPtr {};
          result_ = nullptr;

          // Construct goal
          goal_msg_.pose.pose.position.x = goal_pose_.x;
          goal_msg_.pose.pose.position.y = goal_pose_.y;
          goal_msg_.pose.pose.orientation = rpy_to_quaternion(0.0, 0.0, goal_pose_.theta);

          // Send goal
          auto send_goal_options = rclcpp_action::Client<nav2_msgs::action::NavigateToPose>::SendGoalOptions();
          send_goal_options.goal_response_callback = 
            std::bind(&guide_dog::goal_response_callback, this, std::placeholders::_1);
          send_goal_options.feedback_callback =
            std::bind(&guide_dog::feedback_callback, this, std::placeholders::_1, std::placeholders::_2);
          send_goal_options.result_callback =
            std::bind(&guide_dog::result_callback, this, std::placeholders::_1);
          act_nav_to_pose_->async_send_goal(goal_msg_, send_goal_options);

          state_next_ = State::WAIT_FOR_GOAL_RESPONSE;
        } else {
          RCLCPP_ERROR_STREAM(get_logger(), "Action server not available, aborting.");
          state_next_ = State::IDLE;
        }

        break;
      }
      case State::WAIT_FOR_GOAL_RESPONSE:
      {
        //TODO add timeout
        if (goal_response_received_) {
          if (goal_handle_) {
            RCLCPP_INFO(get_logger(), "Goal accepted by server, waiting for result");
            state_next_ = State::WAIT_FOR_MOVEMENT_COMPLETE;
          } else {
            RCLCPP_ERROR_STREAM(get_logger(), "Goal was rejected by server");
            state_next_ = State::IDLE;
          }
        }

        break;
      }
      case State::WAIT_FOR_MOVEMENT_COMPLETE:
      {
        if (result_) {
          state_next_ = State::IDLE;
        }
        break;
      }
      default:
        auto msg = "Unhandled state: " + get_state_name(state_);
        RCLCPP_ERROR_STREAM(get_logger(), msg);
        throw std::logic_error(msg);
        break;
    }

  }

    /// \brief Voice command topic callback
    void voice_command_callback(const std_msgs::msg::String & msg)
    {
        RCLCPP_ERROR_STREAM(get_logger(), "\n Command \n" << msg.data);

        if (msg.data == "walk")
        {
            if (state_ == State::IDLE)
            {
                // MSR space
                // if (one_ == 0)
                // {
                //     goal_pose_.x = -0.6;
                //     goal_pose_.y = -1.8;
                //     goal_pose_.theta = -1.0;
                //     one_ = 1;
                //     state_next_ = State::SEND_GOAL;
                // }
                // else if (one_ == 1)
                // {
                //     goal_pose_.x = 0.5;
                //     goal_pose_.y = 1.0;
                //     goal_pose_.theta = 1.0;
                //     one_ = 0;
                //     state_next_ = State::SEND_GOAL;
                // }

                // Atrium space
                if (one_ == 0)
                {
                    goal_pose_.x = 2.5;
                    goal_pose_.y = 0.0;
                    goal_pose_.theta = 1.57;
                    one_ = 1;
                    state_next_ = State::SEND_GOAL;
                }
                else if (one_ == 1)
                {
                    goal_pose_.x = 2.5;
                    goal_pose_.y = -2.0;
                    goal_pose_.theta = -1.57;
                    one_ = 2;
                    state_next_ = State::SEND_GOAL;
                }
                else if (one_ == 2)
                {
                    goal_pose_.x = 0.0;
                    goal_pose_.y = 1.0;
                    goal_pose_.theta = 2.35;
                    one_ = 0;
                    state_next_ = State::SEND_GOAL;
                }
                // else if (one_ == 3)
                // {
                //     goal_pose_.x = 0.5;
                //     goal_pose_.y = 1.0;
                //     goal_pose_.theta = 1.0;
                //     one_ = 2;
                //     state_next_ = State::SEND_GOAL;
                // }
            }
        }
        // else if (msg.data == "stop")
        else if (msg.data != "walk") // While walking if you hear anything from voice command STOP
        {
            // Cancel goal service
            RCLCPP_INFO_STREAM(get_logger(), "Cancelling navigation.");
            act_nav_to_pose_->async_cancel_all_goals();
            state_next_ = State::IDLE;
        }
    }

  void srv_nav_to_pose_callback(
    const std::shared_ptr<unitree_nav_interfaces::srv::NavToPose::Request> request,
    std::shared_ptr<unitree_nav_interfaces::srv::NavToPose::Response>
  ) {
    //Store requested pose
    goal_msg_.pose.pose.position.x = request->x;
    goal_msg_.pose.pose.position.y = request->y;
    goal_msg_.pose.pose.orientation = rpy_to_quaternion(0.0, 0.0, request->theta);

    //Initiate action call
    state_next_ = State::SEND_GOAL;
  }

  void cancel_nav_callback(
    const std::shared_ptr<std_srvs::srv::Empty::Request>,
    std::shared_ptr<std_srvs::srv::Empty::Response>
  ) {
    RCLCPP_INFO_STREAM(get_logger(), "Cancelling navigation.");
    act_nav_to_pose_->async_cancel_all_goals();
    state_next_ = State::IDLE;
  }

  void goal_response_callback(
    const rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::SharedPtr & goal_handle
  ) {
    goal_response_received_ = true;
    goal_handle_ = goal_handle;
    RCLCPP_INFO_STREAM(get_logger(), "Goal response");
  }

  void feedback_callback(
    rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::SharedPtr,
    const std::shared_ptr<const nav2_msgs::action::NavigateToPose::Feedback> feedback
  ) {
    //Store result for later use
    feedback_ = feedback;

    if (feedback_) {
      auto [roll, pitch, yaw] = quaternion_to_rpy(feedback_->current_pose.pose.orientation);

      RCLCPP_INFO_STREAM(get_logger(), "x = " << feedback_->current_pose.pose.position.x
                                  << ", y = " << feedback_->current_pose.pose.position.y
                                  << ", theta = " << yaw
      );
    }
  }

  void result_callback(
    const rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::WrappedResult & result
  ) {
    switch (result.code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        RCLCPP_INFO(this->get_logger(), "Goal succeeded");
        break;
      case rclcpp_action::ResultCode::ABORTED:
        RCLCPP_ERROR(this->get_logger(), "Goal was aborted");
        return;
      case rclcpp_action::ResultCode::CANCELED:
        RCLCPP_ERROR(this->get_logger(), "Goal was canceled");
        return;
      default:
        RCLCPP_ERROR(this->get_logger(), "Unknown result code");
        return;
    }

    //Store result for later use
    result_ = std::make_shared<rclcpp_action::ClientGoalHandle<nav2_msgs::action::NavigateToPose>::WrappedResult>();
    *result_ = result;
  }
};

std::tuple<double, double, double> quaternion_to_rpy(const geometry_msgs::msg::Quaternion & q) {
  //https://answers.ros.org/question/339528/quaternion-to-rpy-ros2/
  tf2::Quaternion q_temp;
  tf2::fromMsg(q, q_temp);
  tf2::Matrix3x3 m(q_temp);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);
  return {roll, pitch, yaw};
}

geometry_msgs::msg::Quaternion rpy_to_quaternion(double roll, double pitch, double yaw) {
  tf2::Quaternion q;
  q.setRPY(roll, pitch, yaw);
  return tf2::toMsg(q);
}

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<guide_dog>());
  rclcpp::shutdown();
  return 0;
}








// /// \file
// /// \brief
// ///
// /// PARAMETERS:
// ///
// /// PUBLISHES:
// ///
// /// SUBSCRIBES:
// ///
// /// SERVERS:
// ///
// /// CLIENTS:
// ///     None
// ///
// /// BROADCASTERS:

// #include <chrono>
// #include <functional>
// #include <memory>
// #include <string>
// #include <random>

// #include "rclcpp/rclcpp.hpp"
// #include "std_msgs/msg/string.hpp"
// #include "std_msgs/msg/u_int64.hpp"
// #include "std_srvs/srv/empty.hpp"
// #include "geometry_msgs/msg/transform_stamped.hpp"
// #include "geometry_msgs/msg/point.hpp"
// #include "tf2/LinearMath/Quaternion.h"
// #include "tf2_ros/transform_broadcaster.h"
// #include "unitree_nav_interfaces/srv/set_body_rpy.hpp"
// #include "unitree_nav_interfaces/srv/nav_to_pose.hpp"
// #include "unitree_nav_interfaces/srv/set_body_rpy.hpp"

// using namespace std::chrono_literals;


// /// \brief This class publishes the current timestep of the simulation, obstacles and walls that
// ///#include "unitree_nav_interfaces/srv/nav_to_pose.hpp"
// ///  \param rate (int): Timer callback frequency [Hz]


// class guide_dog : public rclcpp::Node
// {
//     public:
//       guide_dog()
//       : Node("guide_dog"), timestep_(0)
//       {
//         // Parameter descirption
//         auto rate_des = rcl_interfaces::msg::ParameterDescriptor{};
//         rate_des.description = "Timer callback frequency [Hz]";

//         // Declare default parameters values
//         declare_parameter("rate", 200, rate_des);     // Hz for timer_callback

//         // Get params - Read params from yaml file that is passed in the launch file
//         int rate = get_parameter("rate").get_parameter_value().get<int>();

//         // Publishers
//         //Subscribers

//         //Services
//         nav_to_pose_client_ = create_client<unitree_nav_interfaces::srv::NavToPose>("unitree_nav_to_pose");
//         setbodyrpy_client = create_client<unitree_nav_interfaces::srv::SetBodyRPY>("set_body_rpy");

//         // Timer
//         timer_ = create_wall_timer(
//           std::chrono::milliseconds(1000 / rate),
//           std::bind(&guide_dog::timer_callback, this));
//       }

//     private:
//       // Variables
//       size_t timestep_;
//       int one_ = 0;
//       std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster_;

//       // Create objects
//       rclcpp::TimerBase::SharedPtr timer_;
//       rclcpp::Client<unitree_nav_interfaces::srv::NavToPose>::SharedPtr nav_to_pose_client_;
//       rclcpp::Client<unitree_nav_interfaces::srv::SetBodyRPY>::SharedPtr setbodyrpy_client;

//       /// \brief Main simulation timer loop
//       void timer_callback()
//       {
//         if (one_ == 0)
//         {

//             // auto request = std::make_shared<unitree_nav_interfaces::srv::SetBodyRPY::Request>();
//             // request->roll = 0.0;
//             // request->pitch = 0.3;
//             // request->yaw = 0.0;
//             // auto result = setbodyrpy_client->async_send_request(request);
//             // one_ = 1;


//             if (!nav_to_pose_client_->wait_for_service(5s))
//             {
//                 RCLCPP_ERROR_STREAM(get_logger(), "Ready\n");
//                 auto request = std::make_shared<unitree_nav_interfaces::srv::NavToPose::Request>();
//                 request->x = 0.5;
//                 request->y = 0.5;
//                 request->theta = 1.0;
//                 auto result = nav_to_pose_client_->async_send_request(request);
//                 one_ = 1;
//             }

//         }

//       }
// };

// /// \brief Main function for node create, error handel and shutdown
// int main(int argc, char * argv[])
// {
//   rclcpp::init(argc, argv);
//   rclcpp::spin(std::make_shared<guide_dog>());
//   rclcpp::shutdown();
//   return 0;
// }
