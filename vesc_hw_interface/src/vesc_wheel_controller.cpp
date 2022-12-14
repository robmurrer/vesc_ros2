/*********************************************************************
 * Copyright (c) 2022 SoftBank Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ********************************************************************/

#include "vesc_hw_interface/vesc_wheel_controller.h"
#include "ros/forwards.h"

namespace vesc_hw_interface
{
void VescWheelController::init(ros::NodeHandle nh, VescInterface* interface_ptr)
{
  if (interface_ptr == NULL)
  {
    ros::shutdown();
  }
  else
  {
    interface_ptr_ = interface_ptr;
  }

  nh.param<double>("motor/Kp", kp_, 0.005);
  nh.param<double>("motor/Ki", ki_, 0.005);
  nh.param<double>("motor/Kd", kd_, 0.0025);
  nh.param<double>("motor/i_clamp", i_clamp_, 0.2);
  nh.param<double>("motor/duty_limiter", duty_limiter_, 1.0);
  nh.param<bool>("motor/antiwindup", antiwindup_, true);
  nh.param("motor/control_rate", control_rate_, 50.0);

  ROS_INFO("[Motor Gains] P: %f, I: %f, D: %f", kp_, ki_, kd_);
  ROS_INFO("[Motor Gains] I clamp: %f, Antiwindup: %s", i_clamp_, antiwindup_ ? "true" : "false");

  reset_ = true;
  position_sens_ = 0.0;
  velocity_reference_ = 0.0;
  position_pulse_ = 0.0;
  prev_position_pulse_ = 0.0;
  velocity_sens_ = 0.0;
  effort_sens_ = 0.0;

  control_timer_ = nh.createTimer(ros::Duration(1.0 / control_rate_), &VescWheelController::controlTimerCallback, this);
}

void VescWheelController::control(const double target_velocity, const double current_pulse, bool reset)
{
  const double motor_hall_ppr = num_motor_pole_pairs_;
  const double count_deviation_limit = num_motor_pole_pairs_;

  if (reset)
  {
    target_pulse_ = current_pulse;
    error_ = 0.0;
    error_dt_ = 0.0;
    error_integ_ = 0.0;
    error_integ_prev_ = 0.0;
    this->counterTD(current_pulse, true);
  }
  else
  {  // convert rad/s to pulse
    target_pulse_ += target_velocity * motor_hall_ppr / (2 * M_PI) / control_rate_;
  }

  // overflow check
  if (target_pulse_ > static_cast<double>(LONG_MAX))
  {
    target_pulse_ += static_cast<double>(LONG_MIN);
  }
  else if (target_pulse_ < static_cast<double>(LONG_MIN))
  {
    target_pulse_ += static_cast<double>(LONG_MAX);
  }

  // limit error
  if (target_pulse_ - current_pulse > count_deviation_limit)
  {
    target_pulse_ = current_pulse + count_deviation_limit;
  }
  else if (target_pulse_ - current_pulse < -count_deviation_limit)
  {
    target_pulse_ = current_pulse - count_deviation_limit;
  }

  // pid control
  velocity_sens_ = this->counterTD(current_pulse, false) * 2.0 * M_PI / motor_hall_ppr * control_rate_;
  error_dt_ = target_velocity - velocity_sens_;
  error_ = target_pulse_ - current_pulse;
  error_integ_prev_ = error_integ_;
  error_integ_ += (error_ / control_rate_);
  double duty = (kp_ * error_ + ki_ * error_integ_ + kd_ * error_dt_);

  if (antiwindup_)
  {
    if (duty > duty_limiter_)
    {
      duty = duty_limiter_;
      if (error_integ_ > error_integ_prev_)
      {
        error_integ_ = error_integ_prev_;
        duty = (kp_ * error_ + ki_ * error_integ_ + kd_ * error_dt_);
      }
    }
    else if (duty < -duty_limiter_)
    {
      duty = -duty_limiter_;
      if (error_integ_ < error_integ_prev_)
      {
        error_integ_ = error_integ_prev_;
        duty = (kp_ * error_ + ki_ * error_integ_ + kd_ * error_dt_);
      }
    }
    if (ki_ * error_integ_ > i_clamp_)
    {
      error_integ_ = i_clamp_ / ki_;
    }
    else if (ki_ * error_integ_ < -i_clamp_)
    {
      error_integ_ = -i_clamp_ / ki_;
    }
  }

  // limit duty value
  duty = std::clamp(duty, -duty_limiter_, duty_limiter_);
  interface_ptr_->setDutyCycle(fabs(target_velocity) < 0.0001 ? 0.0 : duty);
}

double VescWheelController::counterTD(const double count_in, bool reset)
{
  if (reset)
  {
    counter_changed_single_ = 1;
    for (int i = 0; i < 10; i++)
    {
      counter_changed_log_[i][0] = static_cast<uint16_t>(count_in);
      counter_changed_log_[i][1] = 100;
      counter_td_tmp_[i] = 0;
    }
    return 0.0;
  }

  if (counter_changed_log_[0][0] != static_cast<uint16_t>(count_in))
  {
    for (int i = 0; i < 10; i++)
    {
      counter_changed_log_[10 - i][0] = counter_changed_log_[9 - i][0];
    }
    counter_changed_log_[0][0] = static_cast<uint16_t>(count_in);
    counter_changed_log_[0][1] = counter_changed_single_;
    counter_changed_single_ = 1;
  }
  else
  {
    if (counter_changed_single_ > counter_changed_log_[0][1])
    {
      counter_changed_log_[0][1] = counter_changed_single_;
    }
    if (counter_changed_single_ < 100)
    {
      counter_changed_single_++;
    }
  }

  for (int i = 1; i < 10; i++)
  {
    counter_td_tmp_[10 - i] = counter_td_tmp_[9 - i];
  }
  counter_td_tmp_[0] = static_cast<double>(counter_changed_log_[0][0] - counter_changed_log_[1][0]) /
                       static_cast<double>(counter_changed_log_[0][1]);
  double output = counter_td_tmp_[0];
  if (fabs(output) > 100.0)
  {
    output = 0.0;
  }
  return output;
}

void VescWheelController::setTargetVelocity(const double velocity_reference)
{
  velocity_reference_ = velocity_reference;
}

void VescWheelController::setGearRatio(const double gear_ratio)
{
  gear_ratio_ = gear_ratio;
  ROS_INFO("[VescWheelController]Gear ratio is set to %f", gear_ratio_);
}

void VescWheelController::setTorqueConst(const double torque_const)
{
  torque_const_ = torque_const;
  ROS_INFO("[VescWheelController]Torque constant is set to %f", torque_const_);
}

void VescWheelController::setMotorPolePairs(const int motor_pole_pairs)
{
  num_motor_pole_pairs_ = static_cast<double>(motor_pole_pairs);
  ROS_INFO("[VescWheelController]The number of motor pole pairs is set to %d", motor_pole_pairs);
}

double VescWheelController::getPositionSens()
{
  return position_sens_;
}

double VescWheelController::getVelocitySens()
{
  return velocity_sens_;
}

double VescWheelController::getEffortSens()
{
  return effort_sens_;
}

void VescWheelController::controlTimerCallback(const ros::TimerEvent& e)
{
  double diff = position_pulse_ - prev_position_pulse_;
  if (fabs(diff) > num_motor_pole_pairs_ / 4)
  {
    diff = 0;
    reset_ = true;
  }
  position_sens_ += diff / num_motor_pole_pairs_ * 2.0 * M_PI;

  control(velocity_reference_, position_pulse_, reset_);
  interface_ptr_->requestState();
  reset_ = fabs(velocity_reference_) < 0.0001;
}

void VescWheelController::updateSensor(const std::shared_ptr<const VescPacket>& packet)
{
  if (packet->getName() == "Values")
  {
    std::shared_ptr<VescPacketValues const> values = std::dynamic_pointer_cast<VescPacketValues const>(packet);
    const double current = values->getMotorCurrent();
    const double velocity_rpm = values->getVelocityERPM() / static_cast<double>(num_motor_pole_pairs_);
    prev_position_pulse_ = position_pulse_;
    position_pulse_ = values->getPosition();
    effort_sens_ = current * torque_const_ / gear_ratio_;  // unit: Nm or N
  }
}
}  // namespace vesc_hw_interface