/********************************************************************************
Copyright (c) 2016, Rethink Robotics, Inc.
Copyright (c) 2016, TRACLabs, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

    3. Neither the name of the copyright holder nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
********************************************************************************/

#include <boost/date_time.hpp>
#include <trac_ik/trac_ik.hpp>
#include <sns_ik/sns_ik.hpp>
#include <ros/ros.h>
#include <kdl/chainiksolverpos_nr_jl.hpp>
#include <kdl/chainfksolvervel_recursive.hpp>
#include <kdl/framevel.hpp>
#include <time.h>

double fRand(double min, double max)
{
  double f = (double)rand() / RAND_MAX;
  return min + f * (max - min);
}

double getDeltaWithLimits(double value, double desired_delta,
                          double limit_min, double limit_max)
{
  double valid_delta;
  double lower_delta = value-desired_delta;
  double upper_delta = value+desired_delta;
  double upper_side = (double)rand() / RAND_MAX >= 0.5;
  if( upper_side && upper_delta > limit_max ){
      valid_delta = lower_delta;
  } else if( upper_side && upper_delta <= limit_max) {
      valid_delta = upper_delta;
  } else if( !upper_side && lower_delta < limit_min ){
      valid_delta = upper_delta;
  } else if( !upper_side && lower_delta >= limit_min ){
      valid_delta = lower_delta;
  }
  return valid_delta;
}

bool in_vel_bounds(KDL::JntArray vals, KDL::JntArray vels)
{
  for(size_t i; i < vels.data.size(); i++){
      if(vals(i) < -vels(i) || vals(i) > vels(i)){
          return false;
      }
      /*else{
          std::cout<<vals(i)<<' ';
      }*/
  }
  //std::cout<<std::endl;
  return true;
}

// Compares linear and rotational velocities to see if they are they are scaled properly
bool velocityIsScaled(KDL::FrameVel fv1, KDL::FrameVel fv2, double eps, double *scale)
{
  KDL::Vector v1 = fv1.p.v;
  KDL::Vector v2 = fv2.p.v;
  double v1norm = v1.Norm();
  double v2norm = v2.Norm();
  *scale = v2norm / v1norm;

  // calculate inner product of the velocity vectors
  // theta = acos(cosTheta)
  double cosTheta = KDL::dot(v1, v2) / (v1norm * v2norm);

  // small angle approximation: std::cos(eps) ~ 1 - eps^2/2
  if (cosTheta < 1 - eps*eps/2) {
    return false; // linear velocity not scaled correctly
  }

  // compare rotation scaling
  KDL::Vector w1 = fv1.M.w;
  KDL::Vector w2 = fv2.M.w;
  double w1norm = w1.Norm();
  double w2norm = w2.Norm();
  double rotScale = w2norm / w1norm;

  // Check if the rotation scale matches the linear scale
  if (w1norm > eps && std::fabs(*scale-rotScale) > eps) {
    return false;
  }

  // calculate inner product of the roational velocity vectors
  double cosThetaW = KDL::dot(w1, w2) / (w1norm * w2norm);
  if (cosThetaW < 1 - eps*eps/2) {
    return false; // rotational velocity not scaled correctly
  }

  return true;
}


void test(ros::NodeHandle& nh, double num_samples_pos, double num_samples_vel,
          std::string chain_start, std::string chain_end, double timeout,
          std::string urdf_param, bool randomPositionSeed, bool closePositionSeed)
{

  double eps = 1e-5;
  // This constructor parses the URDF loaded in rosparm urdf_param into the
  // needed KDL structures.  We then pull these out to compare against the KDL
  // IK solver.
  TRAC_IK::TRAC_IK tracik_solver(chain_start, chain_end, urdf_param, timeout, eps);

  KDL::Chain chain;
  KDL::JntArray ll, ul, vl, al; //lower joint limits, upper joint limits

  bool valid = tracik_solver.getKDLChain(chain);

  if (!valid) {
    ROS_ERROR("There was no valid KDL chain found");
    return;
  }

  valid = tracik_solver.getKDLLimits(ll,ul);

  if (!valid) {
    ROS_ERROR("There were no valid KDL joint limits found");
    return;
  }

  assert(chain.getNrOfJoints() == ll.data.size());
  assert(chain.getNrOfJoints() == ul.data.size());

  ROS_INFO ("Using %d joints",chain.getNrOfJoints());


  // Set up KDL IK
  KDL::ChainFkSolverPos_recursive fk_solver(chain); // Forward kin. solver
  KDL::ChainIkSolverVel_pinv vik_solver(chain); // PseudoInverse vel solver
  KDL::ChainIkSolverPos_NR_JL kdl_solver(chain,ll,ul,fk_solver, vik_solver, 1, eps); // Joint Limit Solver
  // 1 iteration per solve (will wrap in timed loop to compare with TRAC-IK)


  // Create Nominal chain configuration midway between all joint limits
  KDL::JntArray nominal(chain.getNrOfJoints());

  for (uint j=0; j<nominal.data.size(); j++) {
    nominal(j) = (ll(j)+ul(j))/2.0;
  }

  // Create desired number of valid, random joint configurations
  std::vector<KDL::JntArray> JointList;
  std::vector<KDL::JntArray> JointDeltaSeed;
  KDL::JntArray q(chain.getNrOfJoints());
  KDL::JntArray q_delta(chain.getNrOfJoints());

  uint num_joint_pos = std::max(num_samples_pos, num_samples_vel);
  for (uint i=0; i < num_joint_pos; i++) {
    for (uint j=0; j<ll.data.size(); j++) {
      q(j)=fRand(ll(j), ul(j));
      q_delta(j)=getDeltaWithLimits(q(j), 0.2, ll(j), ul(j));
    }
    JointList.push_back(q);
    JointDeltaSeed.push_back(q_delta);
  }

  boost::posix_time::ptime start_time;
  boost::posix_time::time_duration diff;

  KDL::JntArray result;
  KDL::Frame end_effector_pose;
  int rc;

  double total_time=0;
  uint success=0;

  ROS_INFO_STREAM("*** Testing KDL with "<<num_samples_pos<<" random samples");

  for (uint i=0; i < num_samples_pos; i++) {
    fk_solver.JntToCart(JointList[i],end_effector_pose);
    double elapsed = 0;
    // set seed
    if (closePositionSeed)
      result = JointDeltaSeed[i];
    else if (i == 0 || !randomPositionSeed)
      result = nominal;
    else
      result = JointList[i-1];
    start_time = boost::posix_time::microsec_clock::local_time();
    do {
      q=result; // when iterating start with last solution
      rc=kdl_solver.CartToJnt(q,end_effector_pose,result);
      diff = boost::posix_time::microsec_clock::local_time() - start_time;
      elapsed = diff.total_nanoseconds() / 1e9;
    } while (rc < 0 && elapsed < timeout);
    total_time+=elapsed;
    if (rc>=0)
      success++;

    if (int((double)i/num_samples_pos*100)%10 == 0)
      ROS_INFO_STREAM_THROTTLE(1,int((i)/num_samples_pos*100)<<"\% done");
  }

  double kdlPos_successRate = success/num_samples_pos;
  double kdlPos_avgTime = total_time/num_samples_pos;
  ROS_INFO_STREAM("KDL found " << success << " solutions (" << 100.0 * kdlPos_successRate
                  << "\%) with an average of " << kdlPos_avgTime << " secs per sample");

  total_time=0;
  success=0;

  ROS_INFO_STREAM("*** Testing TRAC-IK with "<<num_samples_pos<<" random samples");

  for (uint i=0; i < num_samples_pos; i++) {
    fk_solver.JntToCart(JointList[i],end_effector_pose);
    double elapsed = 0;
    if (closePositionSeed)
      q = JointDeltaSeed[i];
    else if (i == 0 || !randomPositionSeed)
      q = nominal;
    else // "random" seed
      q = JointList[i-1];
    start_time = boost::posix_time::microsec_clock::local_time();
    rc=tracik_solver.CartToJnt(q,end_effector_pose,result);
    diff = boost::posix_time::microsec_clock::local_time() - start_time;
    elapsed = diff.total_nanoseconds() / 1e9;
    total_time+=elapsed;
    if (rc>=0)
      success++;

    if (int((double)i/num_samples_pos*100)%10 == 0)
      ROS_INFO_STREAM_THROTTLE(1,int((i)/num_samples_pos*100)<<"\% done");
  }

  double tracPos_successRate = success/num_samples_pos;
  double tracPos_avgTime = total_time/num_samples_pos;
  ROS_INFO_STREAM("TRAC-IK found " << success << " solutions (" << 100.0 * tracPos_successRate
                  << "\%) with an average of " << tracPos_avgTime << " secs per sample");

  sns_ik::SNS_IK snsik_solver(chain_start, chain_end, urdf_param, timeout, eps, sns_ik::SNS);
  valid = snsik_solver.getKDLChain(chain);
  if (!valid) {
    ROS_ERROR("SNS: There was no valid KDL chain found");
    return;
  }
  valid = snsik_solver.getKDLLimits(ll,ul,vl,al);
  if (!valid) {
    ROS_ERROR("SNS: There were no valid KDL joint limits found");
    return;
  }
  assert(chain.getNrOfJoints() == ll.data.size());
  assert(chain.getNrOfJoints() == ul.data.size());
  assert(chain.getNrOfJoints() == vl.data.size());
  assert(chain.getNrOfJoints() == al.data.size());

  // SNS Position Tests
  struct velocitySolverData {
    sns_ik::VelocitySolveType type;
    std::string               name;
    double             successRate;
    double     scaling_successRate;
    double                avg_time;
  };

  std::vector<velocitySolverData> vel_solver_data;
  velocitySolverData sns = {sns_ik::SNS,"SNS",0.0,0.0,0.0};
  vel_solver_data.push_back(sns);
  velocitySolverData sns_optimalsm = {sns_ik::SNS_OptimalScaleMargin,"SNS Optimal Scale Margin",0.0,0.0,0.0};
  vel_solver_data.push_back(sns_optimalsm);
  velocitySolverData sns_optimal = {sns_ik::SNS_Optimal,"SNS Optimal",0.0,0.0,0.0};
  vel_solver_data.push_back(sns_optimal);
  velocitySolverData sns_fast = {sns_ik::SNS_Fast,"SNS Fast",0.0,0.0,0.0};
  vel_solver_data.push_back(sns_fast);
  velocitySolverData sns_fastoptimal = {sns_ik::SNS_FastOptimal,"SNS Fast Optimal",0.0,0.0,0.0};
  vel_solver_data.push_back(sns_fastoptimal);

  // These values are not used yet
  double posIK_linearMaxStepSize = 0.05;
  double posIK_angularMaxStepSize = 0.05;
  double posIK_maxIterations = 150;
  double posIK_dt=0.2;

  for(auto& vst: vel_solver_data){
    snsik_solver.setVelocitySolveType(vst.type);
    // Beginnings of parameter setting. Right now this sets the pos solver to its defaults
    std::shared_ptr<sns_ik::SNSPositionIK> pos_solver;
    snsik_solver.getPositionSolver(pos_solver);
    // pos solver param setting end
    total_time=0;
    success=0;
    ROS_INFO_STREAM("*** Testing SNS-IK with "<<num_samples_pos<<" random samples");
    for (uint i=0; i < num_samples_pos; i++) {
      fk_solver.JntToCart(JointList[i],end_effector_pose);
      double elapsed = 0;
      if (closePositionSeed)
        q = JointDeltaSeed[i];
      else if (i == 0 || !randomPositionSeed)
        q = nominal;
      else // "random" seed
        q = JointList[i-1];
      start_time = boost::posix_time::microsec_clock::local_time();
      rc=snsik_solver.CartToJnt(q,end_effector_pose,result);
      diff = boost::posix_time::microsec_clock::local_time() - start_time;
      elapsed = diff.total_nanoseconds() / 1e9;
      total_time+=elapsed;
      if (rc>=0)
        success++;
      if (int((double)i/num_samples_pos*100)%10 == 0)
        ROS_INFO_STREAM_THROTTLE(1,int((i)/num_samples_pos*100)<<"\% done");
    }
    vst.successRate = success/num_samples_pos;
    vst.avg_time = total_time/num_samples_pos;
    ROS_INFO_STREAM(vst.name << " found " << success << " solutions ("
                    << 100*vst.successRate << "\%) with an average of " << vst.avg_time
                    << " secs per sample");
  }

  ROS_INFO("\n************************************");
  ROS_INFO("Position IK Summary:");
  for(auto& vst: vel_solver_data){
      ROS_INFO("%s: %.2f%% success rate with an average time of %.2f ms",
               vst.name.c_str(), 100*vst.successRate, 1000*vst.avg_time);
  }
  ROS_INFO("KDL: %.2f%% success rate with an average time of %.2f ms",
           100.*kdlPos_successRate, 1000*kdlPos_avgTime);
  ROS_INFO("TRAC: %.2f%% success rate with an average time of %.2f ms",
           100.*tracPos_successRate, 1000*tracPos_avgTime);
  ROS_INFO("\n************************************\n");

  // Create random velocities within the limits
  uint successWithScaling;
  KDL::JntArrayVel result_vel_array;
  KDL::JntArray result_vel;
  KDL::FrameVel end_effector_vel;
  KDL::FrameVel result_end_effector_vel;
  KDL::ChainFkSolverVel_recursive vfk_solver(chain); // Foward kin. vel solver
  std::vector<KDL::JntArrayVel> JointVelList;
  KDL::JntArrayVel v(chain.getNrOfJoints());
  // Create Random velocities within the vel limits
  for (uint i=0; i < num_samples_vel; i++) {
    v.q = JointList[i];
    for (uint j=0; j<vl.data.size(); j++) {
      v.qdot(j)=fRand(-vl(j), vl(j));
    }
    JointVelList.push_back(v);
  }

  for(auto& vst: vel_solver_data){
    total_time=0;
    success=0;
    successWithScaling = 0;
    //ROS_INFO_STREAM("*** Testing SNS-IK Velocity: "<<vst.name);
    snsik_solver.setVelocitySolveType(vst.type);
    for (uint i=0; i < num_samples_vel; i++) {
      // add position to my vel
      vfk_solver.JntToCart(JointVelList[i],end_effector_vel);
      double elapsed = 0;
      start_time = boost::posix_time::microsec_clock::local_time();
      rc=snsik_solver.CartToJnt(JointVelList[i].q, end_effector_vel.GetTwist(), result_vel);

      diff = boost::posix_time::microsec_clock::local_time() - start_time;
      elapsed = diff.total_nanoseconds() / 1e9;
      total_time+=elapsed;

      // check to make sure vel is within limit
      result_vel_array.q = JointVelList[i].q;
      result_vel_array.qdot = result_vel;
      vfk_solver.JntToCart(result_vel_array, result_end_effector_vel);
      bool inVelBounds = in_vel_bounds(result_vel, vl);
      if (rc>=0 && inVelBounds && Equal(end_effector_vel, result_end_effector_vel, 1e-3))
        success++;
      double scale;
      if (rc>=0 && velocityIsScaled(end_effector_vel, result_end_effector_vel, 1e-3, &scale) && inVelBounds)
        successWithScaling++;

      /*if (int((double)i/num_samples_vel*100)%10 == 0)
        ROS_INFO_STREAM_THROTTLE(1,int((i)/num_samples_vel*100)<<"\% done");*/
    }
    //ROS_INFO_STREAM("Velocities Solver found "<<success<<" solutions ("<<100.0*success/num_samples_vel<<"\%) with an average of "<<total_time/num_samples_vel<<" secs per sample");
    //ROS_INFO_STREAM("Velocity Scaling found " << successWithScaling << " solutions ("
    //            << 100.0*successWithScaling/num_samples_vel << "\%)");
    vst.successRate = success/num_samples_vel;
    vst.scaling_successRate = successWithScaling/num_samples_vel;
    vst.avg_time = total_time/num_samples_vel;
  }

  ROS_INFO_STREAM("*** Testing KDL-IK Velocities with " << num_samples_vel << " random samples");
  total_time=0;
  success=0;
  successWithScaling = 0;
  for (uint i=0; i < num_samples_vel; i++) {
    // add position to my vel
    vfk_solver.JntToCart(JointVelList[i],end_effector_vel);
    double elapsed = 0;
    start_time = boost::posix_time::microsec_clock::local_time();
    rc=vik_solver.CartToJnt(JointVelList[i].q, end_effector_vel.GetTwist(), result_vel);
    diff = boost::posix_time::microsec_clock::local_time() - start_time;
    elapsed = diff.total_nanoseconds() / 1e9;
    total_time+=elapsed;
    // check to make sure vel is within limit
    result_vel_array.q = JointVelList[i].q;
    result_vel_array.qdot = result_vel;
    vfk_solver.JntToCart(result_vel_array, result_end_effector_vel);
    bool inVelBounds = in_vel_bounds(result_vel, vl);
    if (rc>=0 && inVelBounds && Equal(end_effector_vel, result_end_effector_vel, 1e-3))
      success++;
    double scale;
    if (rc>=0 && velocityIsScaled(end_effector_vel, result_end_effector_vel, 1e-3, &scale) && inVelBounds)
      successWithScaling++;

    if (int((double)i/num_samples_vel*100)%10 == 0)
      ROS_INFO_STREAM_THROTTLE(1,int((i)/num_samples_vel*100)<<"\% done");
  }

  double kdlVel_successRate = success/num_samples_vel;
  double kdlVel_scalingSuccessRate = successWithScaling/num_samples_vel;
  double kdlVel_avgTime = total_time/num_samples_vel;
  ROS_INFO_STREAM("KDL Velocity found " << success << " solutions (" << 100.0 * kdlVel_successRate
                    << "\%) with an average of " << kdlVel_avgTime << " secs per sample");
  ROS_INFO_STREAM("KDL Velocity Scaling Score " << successWithScaling << " solutions ("
                   << 100.0*kdlVel_scalingSuccessRate << "\%)");

  ROS_INFO("\n************************************");
  ROS_INFO("Velocity IK Summary:");
  for(auto& vst: vel_solver_data){
      ROS_INFO("%s: %.2f%% w/o and %.2f%% w/ scaling success rates with an average time of %.2f ms",
               vst.name.c_str(), 100*vst.successRate, 100*vst.scaling_successRate, 1000*vst.avg_time);
  }
  ROS_INFO("KDL Velocity: %.2f%% w/o and %.2f%% w/ scaling success rates with an average time of %.2f ms",
           100*kdlVel_successRate, 100*kdlVel_scalingSuccessRate, 1000*kdlVel_avgTime);
  ROS_INFO("\n************************************");

}

int main(int argc, char** argv)
{
  srand(time(NULL));
  ros::init(argc, argv, "ik_tests");
  ros::NodeHandle nh("~");

  int num_samples_pos, num_samples_vel;
  std::string chain_start, chain_end, urdf_param;
  double timeout;
  bool randomPositionSeed;
  bool closePositionSeed;
  nh.param("num_samples_pos", num_samples_pos, 100);
  nh.param("num_samples_vel", num_samples_vel, 1000);
  nh.param("chain_start", chain_start, std::string(""));
  nh.param("chain_end", chain_end, std::string(""));
  nh.param("random_position_seed", randomPositionSeed, false);
  nh.param("close_position_seed", closePositionSeed, false);

  if (chain_start=="" || chain_end=="") {
    ROS_FATAL("Missing chain info in launch file");
    exit (-1);
  }

  nh.param("timeout", timeout, 0.005);
  nh.param("urdf_param", urdf_param, std::string("/robot_description"));

  if (num_samples_pos < 1)
    num_samples_pos = 1;

  test(nh, num_samples_pos, num_samples_vel,  chain_start, chain_end, timeout, urdf_param, randomPositionSeed, closePositionSeed);

  // Useful when you make a script that loops over multiple launch files that test different robot chains
  std::vector<char *> commandVector;
  commandVector.push_back((char*)"killall");
  commandVector.push_back((char*)"-9");
  commandVector.push_back((char*)"roslaunch");
  commandVector.push_back(NULL);

  char **command = &commandVector[0];
  execvp(command[0],command);

  return 0;
}
