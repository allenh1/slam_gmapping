/*
 * slam_gmapping
 * Copyright (c) 2008, Willow Garage, Inc.
 *
 * THE WORK (AS DEFINED BELOW) IS PROVIDED UNDER THE TERMS OF THIS CREATIVE
 * COMMONS PUBLIC LICENSE ("CCPL" OR "LICENSE"). THE WORK IS PROTECTED BY
 * COPYRIGHT AND/OR OTHER APPLICABLE LAW. ANY USE OF THE WORK OTHER THAN AS
 * AUTHORIZED UNDER THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.
 * 
 * BY EXERCISING ANY RIGHTS TO THE WORK PROVIDED HERE, YOU ACCEPT AND AGREE TO
 * BE BOUND BY THE TERMS OF THIS LICENSE. THE LICENSOR GRANTS YOU THE RIGHTS
 * CONTAINED HERE IN CONSIDERATION OF YOUR ACCEPTANCE OF SUCH TERMS AND
 * CONDITIONS.
 *
 */

/* Author: Brian Gerkey */
/* Modified by: Charles DuHadway */


/**

@mainpage slam_gmapping

@htmlinclude manifest.html

@b slam_gmapping is a wrapper around the GMapping SLAM library. It reads laser
scans and odometry and computes a map. This map can be
written to a file using e.g.

  "rosrun map_server map_saver static_map:=dynamic_map"

<hr>

@section topic ROS topics

Subscribes to (name/type):
- @b "scan"/<a href="../../sensor_msgs/html/classstd__msgs_1_1LaserScan.html">sensor_msgs/LaserScan</a> : data from a laser range scanner 
- @b "/tf": odometry from the robot


Publishes to (name/type):
- @b "/tf"/tf/tfMessage: position relative to the map


@section services
 - @b "~dynamic_map" : returns the map


@section parameters ROS parameters

Reads the following parameters from the parameter server

Parameters used by our GMapping wrapper:

- @b "~throttle_scans": @b [int] throw away every nth laser scan
- @b "~base_frame": @b [string] the tf frame_id to use for the robot base pose
- @b "~map_frame": @b [string] the tf frame_id where the robot pose on the map is published
- @b "~odom_frame": @b [string] the tf frame_id from which odometry is read
- @b "~map_update_interval": @b [double] time in seconds between two recalculations of the map


Parameters used by GMapping itself:

Laser Parameters:
- @b "~/maxRange" @b [double] maximum range of the laser scans. Rays beyond this range get discarded completely. (default: maximum laser range minus 1 cm, as received in the the first LaserScan message)
- @b "~/maxUrange" @b [double] maximum range of the laser scanner that is used for map building (default: same as maxRange)
- @b "~/sigma" @b [double] standard deviation for the scan matching process (cell)
- @b "~/kernelSize" @b [int] search window for the scan matching process
- @b "~/lstep" @b [double] initial search step for scan matching (linear)
- @b "~/astep" @b [double] initial search step for scan matching (angular)
- @b "~/iterations" @b [int] number of refinement steps in the scan matching. The final "precision" for the match is lstep*2^(-iterations) or astep*2^(-iterations), respectively.
- @b "~/lsigma" @b [double] standard deviation for the scan matching process (single laser beam)
- @b "~/ogain" @b [double] gain for smoothing the likelihood
- @b "~/lskip" @b [int] take only every (n+1)th laser ray for computing a match (0 = take all rays)
- @b "~/minimumScore" @b [double] minimum score for considering the outcome of the scanmatching good. Can avoid 'jumping' pose estimates in large open spaces when using laser scanners with limited range (e.g. 5m). (0 = default. Scores go up to 600+, try 50 for example when experiencing 'jumping' estimate issues)

Motion Model Parameters (all standard deviations of a gaussian noise model)
- @b "~/srr" @b [double] linear noise component (x and y)
- @b "~/stt" @b [double] angular noise component (theta)
- @b "~/srt" @b [double] linear -> angular noise component
- @b "~/str" @b [double] angular -> linear noise component

Others:
- @b "~/linearUpdate" @b [double] the robot only processes new measurements if the robot has moved at least this many meters
- @b "~/angularUpdate" @b [double] the robot only processes new measurements if the robot has turned at least this many rads

- @b "~/resampleThreshold" @b [double] threshold at which the particles get resampled. Higher means more frequent resampling.
- @b "~/particles" @b [int] (fixed) number of particles. Each particle represents a possible trajectory that the robot has traveled

Likelihood sampling (used in scan matching)
- @b "~/llsamplerange" @b [double] linear range
- @b "~/lasamplerange" @b [double] linear step size
- @b "~/llsamplestep" @b [double] linear range
- @b "~/lasamplestep" @b [double] angular step size

Initial map dimensions and resolution:
- @b "~/xmin" @b [double] minimum x position in the map [m]
- @b "~/ymin" @b [double] minimum y position in the map [m]
- @b "~/xmax" @b [double] maximum x position in the map [m]
- @b "~/ymax" @b [double] maximum y position in the map [m]
- @b "~/delta" @b [double] size of one pixel [m]

*/



#include "slam_gmapping.h"

#include <iostream>

#include <time.h>

#include "rclcpp/rclcpp.hpp"
#include "ros2_console/console.hpp"
#include "ros2_console/assert.hpp"
#include "nav_msgs/msg/map_meta_data.hpp"

#include "tf2/utils.h"

#include "gmapping/sensor/sensor_range/rangesensor.h"
#include "gmapping/sensor/sensor_odometry/odometrysensor.h"

// TODO: rosbag does not exist yet
//#include <rosbag/bag.h>
//#include <rosbag/view.h>
#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH

// compute linear index for given map coords
#define MAP_IDX(sx, i, j) ((sx) * (j) + (i))

tf2::Quaternion createQuaternionFromRPY(double roll, double pitch, double yaw)
{
  tf2::Quaternion q;
  q.setRPY(roll, pitch, yaw);
  return q;
}

SlamGMapping::SlamGMapping(rclcpp::node::Node::SharedPtr nh, rclcpp::node::Node::SharedPtr pnh):
  map_to_odom_(tf2::Transform(createQuaternionFromRPY( 0, 0, 0 ), tf2::Vector3(0, 0, 0 ))),
  laser_count_(0),node_(nh), private_nh_(pnh),
  transform_thread_(NULL),
  tf_(buffer_)
{
  seed_ = time(NULL);
  init();
}

SlamGMapping::SlamGMapping(
  rclcpp::node::Node::SharedPtr nh,
  rclcpp::node::Node::SharedPtr pnh,
  long unsigned int seed,
  long unsigned int max_duration_buffer)
  :
  map_to_odom_(tf2::Transform(createQuaternionFromRPY( 0, 0, 0 ), tf2::Vector3(0, 0, 0 ))),
  laser_count_(0), node_(nh), private_nh_(pnh),
  transform_thread_(NULL),
  seed_(seed), buffer_(tf2::Duration(max_duration_buffer)), tf_(buffer_)
{
  init();
}


void SlamGMapping::init()
{
  // The library is pretty chatty
  //gsp_ = new GMapping::GridSlamProcessor(std::cerr);
  gsp_ = new GMapping::GridSlamProcessor();
  ROS_ASSERT(gsp_);

  tfB_ = new tf2_ros::TransformBroadcaster(node_);
  ROS_ASSERT(tfB_);
  
  gsp_laser_ = NULL;
  gsp_odom_ = NULL;

  got_first_scan_ = false;
  got_map_ = false;
  
  // TODO: fix all parameters below once parameters are fully supported
  
  // Parameters used by our GMapping wrapper
  //if(!private_nh_.getParam("throttle_scans", throttle_scans_))
  throttle_scans_ = 1;
  //if(!private_nh_.getParam("base_frame", base_frame_))
  base_frame_ = "base_link";
  //if(!private_nh_.getParam("map_frame", map_frame_))
  map_frame_ = "map";
  //if(!private_nh_.getParam("odom_frame", odom_frame_))
  odom_frame_ = "odom";

  //private_nh_.param("transform_publish_period", transform_publish_period_, 0.05);
  transform_publish_period_ = 0.05;

  double tmp;
  //if(!private_nh_.getParam("map_update_interval", tmp))
  tmp = 5.0;
  map_update_interval_ = tf2::durationFromSec(tmp);
  
  // Parameters used by GMapping itself
  maxUrange_ = 0.0;  maxRange_ = 0.0; // preliminary default, will be set in initMapper()
  //if(!private_nh_.getParam("minimumScore", minimum_score_))
  minimum_score_ = 0;
  //if(!private_nh_.getParam("sigma", sigma_))
  sigma_ = 0.05;
  //if(!private_nh_.getParam("kernelSize", kernelSize_))
  kernelSize_ = 1;
  //if(!private_nh_.getParam("lstep", lstep_))
  lstep_ = 0.05;
  //if(!private_nh_.getParam("astep", astep_))
  astep_ = 0.05;
  //if(!private_nh_.getParam("iterations", iterations_))
  iterations_ = 5;
  //if(!private_nh_.getParam("lsigma", lsigma_))
  lsigma_ = 0.075;
  //if(!private_nh_.getParam("ogain", ogain_))
  ogain_ = 3.0;
  //if(!private_nh_.getParam("lskip", lskip_))
  lskip_ = 0;
  //if(!private_nh_.getParam("srr", srr_))
  srr_ = 0.1;
  //if(!private_nh_.getParam("srt", srt_))
  srt_ = 0.2;
  //if(!private_nh_.getParam("str", str_))
  str_ = 0.1;
  //if(!private_nh_.getParam("stt", stt_))
  stt_ = 0.2;
  //if(!private_nh_.getParam("linearUpdate", linearUpdate_))
  linearUpdate_ = 1.0;
  //if(!private_nh_.getParam("angularUpdate", angularUpdate_))
  angularUpdate_ = 0.5;
  //if(!private_nh_.getParam("temporalUpdate", temporalUpdate_))
  temporalUpdate_ = -1.0;
  //if(!private_nh_.getParam("resampleThreshold", resampleThreshold_))
  resampleThreshold_ = 0.5;
  //if(!private_nh_.getParam("particles", particles_))
  particles_ = 30;
  //if(!private_nh_.getParam("xmin", xmin_))
  xmin_ = -100.0;
  //if(!private_nh_.getParam("ymin", ymin_))
  ymin_ = -100.0;
  //if(!private_nh_.getParam("xmax", xmax_))
  xmax_ = 100.0;
  //if(!private_nh_.getParam("ymax", ymax_))
  ymax_ = 100.0;
  //if(!private_nh_.getParam("delta", delta_))
  delta_ = 0.05;
  //if(!private_nh_.getParam("occ_thresh", occ_thresh_))
  occ_thresh_ = 0.25;
  //if(!private_nh_.getParam("llsamplerange", llsamplerange_))
  llsamplerange_ = 0.01;
  //if(!private_nh_.getParam("llsamplestep", llsamplestep_))
  llsamplestep_ = 0.01;
  //if(!private_nh_.getParam("lasamplerange", lasamplerange_))
  lasamplerange_ = 0.005;
  //if(!private_nh_.getParam("lasamplestep", lasamplestep_))
  lasamplestep_ = 0.005;
    
  //if(!private_nh_.getParam("tf_delay", tf_delay_))
  tf_delay_ = transform_publish_period_;
}


void SlamGMapping::startLiveSlam()
{
  // TODO: make entropy_publisher_ latched
  entropy_publisher_ = private_nh_->create_publisher<std_msgs::msg::Float64>("entropy", 1);

  // TODO: make sst_ latched
  sst_ = node_->create_publisher<nav_msgs::msg::OccupancyGrid>("map");
  
  // TODO: make sstm_ latched
  sstm_ = node_->create_publisher<nav_msgs::msg::MapMetaData>("map_metadata", 1);
  ss_ = node_->create_service<nav_msgs::srv::GetMap>(
      "dynamic_map",
      std::bind(&SlamGMapping::mapCallback, this,
		std::placeholders::_1,
		std::placeholders::_2,
		std::placeholders::_3));
  scan_sub_ = node_->create_subscription<sensor_msgs::msg::LaserScan>(
      "scan", 5, std::bind(&SlamGMapping::laserCallback, this, std::placeholders::_1));

  transform_thread_ = new boost::thread(boost::bind(&SlamGMapping::publishLoop, this, transform_publish_period_));
}

void SlamGMapping::startReplay(const std::string & bag_fname, std::string scan_topic)
{
  throw std::runtime_error("The SlamGMapping::startReplay function has not been implemented yet");

  // TODO: implement once rosbag exists
  /*
  double transform_publish_period;
  rclcpp::node::Node::sharedPtr private_nh_ = rclcpp::node::Node::make_shared("slam_gmapping");
  entropy_publisher_ = private_nh_.advertise<std_msgs::msg::Float64>("entropy", 1, true);
  sst_ = node_.advertise<nav_msgs::msg::OccupancyGrid>("map", 1, true);
  sstm_ = node_.advertise<nav_msgs::msg::MapMetaData>("map_metadata", 1, true);
  ss_ = node_.advertiseService("dynamic_map", &SlamGMapping::mapCallback, this);
  
  rosbag::Bag bag;
  bag.open(bag_fname, rosbag::bagmode::Read);
  
  std::vector<std::string> topics;
  topics.push_back(std::string("/tf"));
  topics.push_back(scan_topic);
  rosbag::View viewall(bag, rosbag::TopicQuery(topics));

  // Store up to 5 messages and there error message (if they cannot be processed right away)
  std::queue<std::pair<sensor_msgs::msg::LaserScan::ConstPtr, std::string> > s_queue;
  foreach(rosbag::MessageInstance const m, viewall)
  {
    tf::tfMessage::ConstPtr cur_tf = m.instantiate<tf::tfMessage>();
    if (cur_tf != NULL) {
      for (size_t i = 0; i < cur_tf->transforms.size(); ++i)
      {
        geometry_msgs::msg::TransformStamped transformStamped;
        tf2::StampedTransform stampedTf;
        transformStamped = cur_tf->transforms[i];
        tf::transformStampedMsgToTF(transformStamped, stampedTf);
        buffer_.setTransform(transformStamped);
      }
    }

    sensor_msgs::msg::LaserScan::ConstPtr s = m.instantiate<sensor_msgs::msg::LaserScan>();
    if (s != NULL) {
      tf2::TimePoint tp = tf2::TimePoint(
        std::chrono::seconds(s->header.stamp.sec) + std::chrono::nanoseconds(s->header.stamp.nanosec));
      if (!(tp == tf2::TimePointZero)
      {
        s_queue.push(std::make_pair(s, ""));
      }
      // Just like in live processing, only process the latest 5 scans
      if (s_queue.size() > 5) {
        ROS_WARN_STREAM("Dropping old scan: " << s_queue.front().second);
        s_queue.pop();
      }
      // ignoring un-timestamped tf data 
    }

    // Only process a scan if it has tf data
    while (!s_queue.empty())
    {
      try
      {
        tf2::StampedTransform t;
        buffer_.lookupTransform(s_queue.front().first->header.frame_id, odom_frame_, s_queue.front().first->header.stamp, t);
        this->laserCallback(s_queue.front().first);
        s_queue.pop();
      }
      // If tf does not have the data yet
      catch(tf2::TransformException& e)
      {
        // Store the error to display it if we cannot process the data after some time
        s_queue.front().second = std::string(e.what());
        break;
      }
    }
  }

  bag.close();
  */
}

void SlamGMapping::publishLoop(double transform_publish_period){
  if(transform_publish_period == 0)
    return;

  rclcpp::rate::Rate r(1.0 / transform_publish_period);
  while(rclcpp::ok()){
    publishTransform();
    r.sleep();
  }
}

SlamGMapping::~SlamGMapping()
{
  if(transform_thread_){
    transform_thread_->join();
    delete transform_thread_;
  }

  delete gsp_;
  if(gsp_laser_)
    delete gsp_laser_;
  if(gsp_odom_)
    delete gsp_odom_;
}

bool
SlamGMapping::getOdomPose(GMapping::OrientedPoint& gmap_pose, const builtin_interfaces::msg::Time& t)
{
  tf2::TimePoint tp = tf2::TimePoint(
      std::chrono::seconds(t.sec) + std::chrono::nanoseconds(t.nanosec));
    
  tf2::Transform odom_pose;
  try
  {
    // Look up the laser's pose that is centered
    geometry_msgs::msg::TransformStamped transMsg = buffer_.lookupTransform(
        odom_frame_,
	laser_frame_,
	tp,
	tf2::durationFromSec(0.1));

    // Convert message to tf2 type
    tf2::Transform transform(
        tf2::Quaternion(
            transMsg.transform.rotation.x,
	    transMsg.transform.rotation.y,
	    transMsg.transform.rotation.z,
	    transMsg.transform.rotation.w),
	tf2::Vector3(
	    transMsg.transform.translation.x,
	    transMsg.transform.translation.y,
	    transMsg.transform.translation.z));
    
    // Transform the position
    odom_pose = transform * centered_laser_pose_;
  }
  catch(tf2::TransformException e)
  {
    ROS_WARN("Failed to compute odom pose, skipping scan (%s)", e.what());
    return false;
  }

  double yaw = 0;
  tf2Scalar uselessRoll, uselessPitch;
  tf2::Matrix3x3(odom_pose.getRotation()).getRPY(uselessRoll, uselessPitch, yaw);

  gmap_pose = GMapping::OrientedPoint(odom_pose.getOrigin().x(),
                                      odom_pose.getOrigin().y(),
                                      yaw);
  return true;
}

bool
SlamGMapping::initMapper(const sensor_msgs::msg::LaserScan& scan)
{
  laser_frame_ = scan.header.frame_id;
  // Get the laser's pose, relative to base.
  tf2::Transform ident;
  tf2::Transform laser_pose;
  ident.setIdentity();

  tf2::TimePoint tp = tf2::TimePoint(
      std::chrono::seconds(scan.header.stamp.sec) +
      std::chrono::nanoseconds(scan.header.stamp.nanosec));
   
  try
  {
    geometry_msgs::msg::TransformStamped transMsg = buffer_.lookupTransform(
        base_frame_,
	laser_frame_,
	tp,
	tf2::durationFromSec(0.1));

    // Convert message to tf2 type
    tf2::Transform transform(
        tf2::Quaternion(
            transMsg.transform.rotation.x,
	    transMsg.transform.rotation.y,
	    transMsg.transform.rotation.z,
	    transMsg.transform.rotation.w),
	tf2::Vector3(
	    transMsg.transform.translation.x,
	    transMsg.transform.translation.y,
	    transMsg.transform.translation.z));
    
    // Transform the position
    laser_pose = transform * ident;
  }
  catch(tf2::TransformException e)
  {
    ROS_WARN("Failed to compute laser pose, aborting initialization (%s)",
             e.what());
    return false;
  }

  // create a point 1m above the laser position and transform it into the laser-frame
  tf2::Vector3 v;
  v.setValue(0, 0, 1 + laser_pose.getOrigin().z());
  tf2::Vector3 up;
  try
  {
    geometry_msgs::msg::TransformStamped transMsg = buffer_.lookupTransform(
        base_frame_,
	laser_frame_,
	tp,
	tf2::durationFromSec(0.1));

    // Convert message to tf2 type
    tf2::Transform transform(
        tf2::Quaternion(
            transMsg.transform.rotation.x,
	    transMsg.transform.rotation.y,
	    transMsg.transform.rotation.z,
	    transMsg.transform.rotation.w),
	tf2::Vector3(
	    transMsg.transform.translation.x,
	    transMsg.transform.translation.y,
	    transMsg.transform.translation.z));

    up = transform * v;
    
    ROS_DEBUG("Z-Axis in sensor frame: %.3f", up.z());
  }
  catch(tf2::TransformException& e)
  {
    ROS_WARN("Unable to determine orientation of laser: %s",
             e.what());
    return false;
  }
  
  // gmapping doesnt take roll or pitch into account. So check for correct sensor alignment.
  if (fabs(fabs(up.z()) - 1) > 0.001)
  {
    ROS_WARN("Laser has to be mounted planar! Z-coordinate has to be 1 or -1, but gave: %.5f",
                 up.z());
    return false;
  }

  gsp_laser_beam_count_ = scan.ranges.size();

  double angle_center = (scan.angle_min + scan.angle_max)/2;

  if (up.z() > 0)
  {
    do_reverse_range_ = scan.angle_min > scan.angle_max;
    centered_laser_pose_ = tf2::Transform(createQuaternionFromRPY(0,0,angle_center), tf2::Vector3(0,0,0));
    ROS_INFO("Laser is mounted upwards.");
  }
  else
  {
    do_reverse_range_ = scan.angle_min < scan.angle_max;
    centered_laser_pose_ = tf2::Transform(createQuaternionFromRPY(M_PI,0,-angle_center), tf2::Vector3(0,0,0));
    ROS_INFO("Laser is mounted upside down.");
  }

  // Compute the angles of the laser from -x to x, basically symmetric and in increasing order
  laser_angles_.resize(scan.ranges.size());
  // Make sure angles are started so that they are centered
  double theta = - std::fabs(scan.angle_min - scan.angle_max)/2;
  for(unsigned int i=0; i<scan.ranges.size(); ++i)
  {
    laser_angles_[i]=theta;
    theta += std::fabs(scan.angle_increment);
  }

  ROS_DEBUG("Laser angles in laser-frame: min: %.3f max: %.3f inc: %.3f", scan.angle_min, scan.angle_max,
            scan.angle_increment);
  ROS_DEBUG("Laser angles in top-down centered laser-frame: min: %.3f max: %.3f inc: %.3f", laser_angles_.front(),
            laser_angles_.back(), std::fabs(scan.angle_increment));

  GMapping::OrientedPoint gmap_pose(0, 0, 0);

  // setting maxRange and maxUrange here so we can set a reasonable default
  rclcpp::node::Node::SharedPtr private_nh_ = rclcpp::node::Node::make_shared("slam_gmapping");

  // TODO: fix parameters once they are fully supported
  //if(!private_nh_.getParam("maxRange", maxRange_))
  maxRange_ = scan.range_max - 0.01;
  //if(!private_nh_.getParam("maxUrange", maxUrange_))
  maxUrange_ = maxRange_;

  // The laser must be called "FLASER".
  // We pass in the absolute value of the computed angle increment, on the
  // assumption that GMapping requires a positive angle increment.  If the
  // actual increment is negative, we'll swap the order of ranges before
  // feeding each scan to GMapping.
  gsp_laser_ = new GMapping::RangeSensor("FLASER",
                                         gsp_laser_beam_count_,
                                         fabs(scan.angle_increment),
                                         gmap_pose,
                                         0.0,
                                         maxRange_);
  ROS_ASSERT(gsp_laser_);

  GMapping::SensorMap smap;
  smap.insert(make_pair(gsp_laser_->getName(), gsp_laser_));
  gsp_->setSensorMap(smap);

  gsp_odom_ = new GMapping::OdometrySensor(odom_frame_);
  ROS_ASSERT(gsp_odom_);


  /// @todo Expose setting an initial pose
  GMapping::OrientedPoint initialPose;
  if(!getOdomPose(initialPose, scan.header.stamp))
  {
    ROS_WARN("Unable to determine inital pose of laser! Starting point will be set to zero.");
    initialPose = GMapping::OrientedPoint(0.0, 0.0, 0.0);
  }

  gsp_->setMatchingParameters(maxUrange_, maxRange_, sigma_,
                              kernelSize_, lstep_, astep_, iterations_,
                              lsigma_, ogain_, lskip_);

  gsp_->setMotionModelParameters(srr_, srt_, str_, stt_);
  gsp_->setUpdateDistances(linearUpdate_, angularUpdate_, resampleThreshold_);
  gsp_->setUpdatePeriod(temporalUpdate_);
  gsp_->setgenerateMap(false);
  gsp_->GridSlamProcessor::init(particles_, xmin_, ymin_, xmax_, ymax_,
                                delta_, initialPose);
  gsp_->setllsamplerange(llsamplerange_);
  gsp_->setllsamplestep(llsamplestep_);
  /// @todo Check these calls; in the gmapping gui, they use
  /// llsamplestep and llsamplerange intead of lasamplestep and
  /// lasamplerange.  It was probably a typo, but who knows.
  gsp_->setlasamplerange(lasamplerange_);
  gsp_->setlasamplestep(lasamplestep_);
  gsp_->setminimumScore(minimum_score_);

  // Call the sampling function once to set the seed.
  GMapping::sampleGaussian(1,seed_);

  ROS_INFO("Initialization complete");

  return true;
}

bool
SlamGMapping::addScan(const sensor_msgs::msg::LaserScan& scan, GMapping::OrientedPoint& gmap_pose)
{
  if(!getOdomPose(gmap_pose, scan.header.stamp)) {
     return false;
  }
  
  if(scan.ranges.size() != gsp_laser_beam_count_) {
    return false;
  }

  // GMapping wants an array of doubles...
  double* ranges_double = new double[scan.ranges.size()];
  // If the angle increment is negative, we have to invert the order of the readings.
  if (do_reverse_range_)
  {
    ROS_DEBUG("Inverting scan");
    int num_ranges = scan.ranges.size();
    for(int i=0; i < num_ranges; i++)
    {
      // Must filter out short readings, because the mapper won't
      if(scan.ranges[num_ranges - i - 1] < scan.range_min)
        ranges_double[i] = (double)scan.range_max;
      else
        ranges_double[i] = (double)scan.ranges[num_ranges - i - 1];
    }
  } else 
  {
    for(unsigned int i=0; i < scan.ranges.size(); i++)
    {
      // Must filter out short readings, because the mapper won't
      if(scan.ranges[i] < scan.range_min)
        ranges_double[i] = (double)scan.range_max;
      else
        ranges_double[i] = (double)scan.ranges[i];
    }
  }

  tf2::TimePoint tp = tf2::TimePoint(
      std::chrono::seconds(scan.header.stamp.sec) +
      std::chrono::nanoseconds(scan.header.stamp.nanosec));
  
  GMapping::RangeReading reading(scan.ranges.size(),
                                 ranges_double,
                                 gsp_laser_,
                                 tf2::timeToSec(tp));

  // ...but it deep copies them in RangeReading constructor, so we don't
  // need to keep our array around.
  delete[] ranges_double;

  reading.setPose(gmap_pose);

  /*
  ROS_DEBUG("scanpose (%.3f): %.3f %.3f %.3f\n",
            scan.header.stamp.toSec(),
            gmap_pose.x,
            gmap_pose.y,
            gmap_pose.theta);
            */
  ROS_DEBUG("processing scan");

  return gsp_->processScan(reading);
}

void
SlamGMapping::laserCallback(const sensor_msgs::msg::LaserScan::SharedPtr scan)
{
  tf2::TimePoint tp = tf2::TimePoint(
      std::chrono::seconds(scan->header.stamp.sec) +
      std::chrono::nanoseconds(scan->header.stamp.nanosec));
  
  // Skip any messages that can not be transformed to the odometry frame
  if (!buffer_.canTransform(odom_frame_,
			    scan->header.frame_id,
			    tp,
			    tf2::durationFromSec(0.1))) {
    return;
  }
  
  laser_count_++;
  if ((laser_count_ % throttle_scans_) != 0)
    return;

  static tf2::TimePoint last_map_update = tf2::TimePointZero;

  // We can't initialize the mapper until we've got the first scan
  if(!got_first_scan_)
  {
    if(!initMapper(*scan))
      return;
    got_first_scan_ = true;
  }

  GMapping::OrientedPoint odom_pose;

  if(addScan(*scan, odom_pose))
  {
    ROS_DEBUG("scan processed");

    GMapping::OrientedPoint mpose = gsp_->getParticles()[gsp_->getBestParticleIndex()].pose;
    ROS_DEBUG("new best pose: %.3f %.3f %.3f", mpose.x, mpose.y, mpose.theta);
    ROS_DEBUG("odom pose: %.3f %.3f %.3f", odom_pose.x, odom_pose.y, odom_pose.theta);
    ROS_DEBUG("correction: %.3f %.3f %.3f", mpose.x - odom_pose.x, mpose.y - odom_pose.y, mpose.theta - odom_pose.theta);

    tf2::Transform laser_to_map = tf2::Transform(createQuaternionFromRPY(0, 0, mpose.theta), tf2::Vector3(mpose.x, mpose.y, 0.0)).inverse();
    tf2::Transform odom_to_laser = tf2::Transform(createQuaternionFromRPY(0, 0, odom_pose.theta), tf2::Vector3(odom_pose.x, odom_pose.y, 0.0));

    map_to_odom_mutex_.lock();
    map_to_odom_ = (odom_to_laser * laser_to_map).inverse();
    map_to_odom_mutex_.unlock();

    tf2::TimePoint tp = tf2::TimePoint(
        std::chrono::seconds(scan->header.stamp.sec) +
        std::chrono::nanoseconds(scan->header.stamp.nanosec));

    if(!got_map_ || (tp - last_map_update) > map_update_interval_)
    {
      updateMap(*scan);
      last_map_update = tp;
      ROS_DEBUG("Updated the map");
    }
  } else
    ROS_DEBUG("cannot process scan");
}

double
SlamGMapping::computePoseEntropy()
{
  double weight_total=0.0;
  for(std::vector<GMapping::GridSlamProcessor::Particle>::const_iterator it = gsp_->getParticles().begin();
      it != gsp_->getParticles().end();
      ++it)
  {
    weight_total += it->weight;
  }
  double entropy = 0.0;
  for(std::vector<GMapping::GridSlamProcessor::Particle>::const_iterator it = gsp_->getParticles().begin();
      it != gsp_->getParticles().end();
      ++it)
  {
    if(it->weight/weight_total > 0.0)
      entropy += it->weight/weight_total * log(it->weight/weight_total);
  }
  return -entropy;
}

void
SlamGMapping::updateMap(const sensor_msgs::msg::LaserScan& scan)
{
  ROS_DEBUG("Update map");
  boost::mutex::scoped_lock map_lock (map_mutex_);
  GMapping::ScanMatcher matcher;

  matcher.setLaserParameters(scan.ranges.size(), &(laser_angles_[0]),
                             gsp_laser_->getPose());

  matcher.setlaserMaxRange(maxRange_);
  matcher.setusableRange(maxUrange_);
  matcher.setgenerateMap(true);

  GMapping::GridSlamProcessor::Particle best =
          gsp_->getParticles()[gsp_->getBestParticleIndex()];
  std_msgs::msg::Float64 entropy;
  entropy.data = computePoseEntropy();
  if(entropy.data > 0.0)
    entropy_publisher_->publish(entropy);

  if(!got_map_) {
    map_.map.info.resolution = delta_;
    map_.map.info.origin.position.x = 0.0;
    map_.map.info.origin.position.y = 0.0;
    map_.map.info.origin.position.z = 0.0;
    map_.map.info.origin.orientation.x = 0.0;
    map_.map.info.origin.orientation.y = 0.0;
    map_.map.info.origin.orientation.z = 0.0;
    map_.map.info.origin.orientation.w = 1.0;
  } 

  GMapping::Point center;
  center.x=(xmin_ + xmax_) / 2.0;
  center.y=(ymin_ + ymax_) / 2.0;

  GMapping::ScanMatcherMap smap(center, xmin_, ymin_, xmax_, ymax_, 
                                delta_);

  ROS_DEBUG("Trajectory tree:");
  for(GMapping::GridSlamProcessor::TNode* n = best.node;
      n;
      n = n->parent)
  {
    ROS_DEBUG("  %.3f %.3f %.3f",
              n->pose.x,
              n->pose.y,
              n->pose.theta);
    if(!n->reading)
    {
      ROS_DEBUG("Reading is NULL");
      continue;
    }
    matcher.invalidateActiveArea();
    matcher.computeActiveArea(smap, n->pose, &((*n->reading)[0]));
    matcher.registerScan(smap, n->pose, &((*n->reading)[0]));
  }

  // the map may have expanded, so resize ros message as well
  if(map_.map.info.width != (unsigned int) smap.getMapSizeX() || map_.map.info.height != (unsigned int) smap.getMapSizeY()) {

    // NOTE: The results of ScanMatcherMap::getSize() are different from the parameters given to the constructor
    //       so we must obtain the bounding box in a different way
    GMapping::Point wmin = smap.map2world(GMapping::IntPoint(0, 0));
    GMapping::Point wmax = smap.map2world(GMapping::IntPoint(smap.getMapSizeX(), smap.getMapSizeY()));
    xmin_ = wmin.x; ymin_ = wmin.y;
    xmax_ = wmax.x; ymax_ = wmax.y;
    
    ROS_DEBUG("map size is now %dx%d pixels (%f,%f)-(%f, %f)", smap.getMapSizeX(), smap.getMapSizeY(),
              xmin_, ymin_, xmax_, ymax_);

    map_.map.info.width = smap.getMapSizeX();
    map_.map.info.height = smap.getMapSizeY();
    map_.map.info.origin.position.x = xmin_;
    map_.map.info.origin.position.y = ymin_;
    map_.map.data.resize(map_.map.info.width * map_.map.info.height);

    ROS_DEBUG("map origin: (%f, %f)", map_.map.info.origin.position.x, map_.map.info.origin.position.y);
  }

  for(int x=0; x < smap.getMapSizeX(); x++)
  {
    for(int y=0; y < smap.getMapSizeY(); y++)
    {
      /// @todo Sort out the unknown vs. free vs. obstacle thresholding
      GMapping::IntPoint p(x, y);
      double occ=smap.cell(p);
      assert(occ <= 1.0);
      if(occ < 0)
        map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = -1;
      else if(occ > occ_thresh_)
      {
        //map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = (int)round(occ*100.0);
        map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = 100;
      }
      else
        map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = 0;
    }
  }
  got_map_ = true;

  //make sure to set the header information on the map
  map_.map.header.stamp = tf2_ros::toMsg(tf2::get_now());
  map_.map.header.frame_id = map_frame_;  // TODO: need tf resolve tf_.resolve( map_frame_ );

  sst_->publish(map_.map);
  sstm_->publish(map_.map.info);
}

bool 
SlamGMapping::mapCallback(
    const std::shared_ptr<rmw_request_id_t> request_header,
    const std::shared_ptr<nav_msgs::srv::GetMap::Request> req,
    std::shared_ptr<nav_msgs::srv::GetMap::Response> res)
{
  boost::mutex::scoped_lock map_lock (map_mutex_);
  if(got_map_ && map_.map.info.width && map_.map.info.height)
  {
    res->map = map_.map;
    return true;
  }
  else
    return false;
}

void SlamGMapping::publishTransform()
{
  map_to_odom_mutex_.lock();
  tf2::TimePoint tf_expiration = tf2::get_now() + tf2::durationFromSec(tf_delay_);
  
  geometry_msgs::msg::TransformStamped msg;
  msg.header.frame_id = map_frame_;
  msg.header.stamp = tf2_ros::toMsg(tf_expiration);
  msg.child_frame_id = odom_frame_;
  msg.transform.translation.x = map_to_odom_.getOrigin().x();
  msg.transform.translation.y = map_to_odom_.getOrigin().y();
  msg.transform.translation.z = map_to_odom_.getOrigin().z();
  msg.transform.rotation.x = map_to_odom_.getRotation().x();
  msg.transform.rotation.y = map_to_odom_.getRotation().y();
  msg.transform.rotation.z = map_to_odom_.getRotation().z();
  msg.transform.rotation.w = map_to_odom_.getRotation().w();
  
  tfB_->sendTransform(msg);
  
  map_to_odom_mutex_.unlock();
}
