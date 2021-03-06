/*
 * slam_gmapping
 * Copyright (c) 2008, Willow Garage, Inc.
 * Copyright (c) 2017, Open Source Robotics Foundation, Inc.
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


#include "slam_gmapping.hpp"
/* #include "ros/console.h" */

/* #include <rosbag/bag.h> */
/* #include <rosbag/view.h> */

// compute linear index for given map coords
#define MAP_IDX(sx, i, j) ((sx) * (j) + (i))

SlamGMapping::SlamGMapping(const rclcpp::NodeOptions & options)
: rclcpp::Node("slam_gmapping", options)
{
  tfB_ = std::make_unique<tf2_ros::TransformBroadcaster>(this);
  buffer = std::make_unique<tf2_ros::Buffer>(this->get_clock());
  /* buffer->setUsingDedicatedThread(true); */
  tf_node_ = std::make_shared<rclcpp::Node>("slam_gmapping_tf", options);
  tf_ = std::make_unique<tf2_ros::TransformListener>(*buffer, tf_node_, true);
  map_to_odom_.setIdentity();

  seed_ = time(NULL);

  gsp_ = new GMapping::GridSlamProcessor(std::cerr);
  if (!gsp_) {
    RCLCPP_ERROR(this->get_logger(), "Failed to allocate GridSlamProcessor!");
    exit(1);
  } else if (!tfB_) {
    RCLCPP_ERROR(this->get_logger(), "Failed to allocate transform buffer!");
    exit(2);
  }

  init();
  startLiveSlam();
}

SlamGMapping::~SlamGMapping()
{
  delete gsp_;
  if (gsp_laser_) {
    delete gsp_laser_;
  }
}

void SlamGMapping::init()
{
  // log4cxx::Logger::getLogger(ROSCONSOLE_DEFAULT_NAME)->setLevel(ros::console::g_level_lookup[ros::console::levels::Debug]);

  // The library is pretty chatty
  //gsp_ = new GMapping::GridSlamProcessor(std::cerr);
  // Parameters used by our GMapping wrapper
  throttle_scans_ = this->declare_parameter("throttle_scans", 1);
  base_frame_ = this->declare_parameter("base_frame", std::string("base_link"));
  map_frame_ = this->declare_parameter("map_frame", std::string("map"));
  odom_frame_ = this->declare_parameter("odom_frame", std::string("odom"));

  transform_publish_period_ = this->declare_parameter("transform_publish_period", 0.05);

  double tmp;
  tmp = this->declare_parameter("map_update_interval",  5.0);
  map_update_interval_ = tf2::durationFromSec(tmp);

  // Parameters used by GMapping itself
  maxUrange_ = 0.0;  maxRange_ = 0.0; // preliminary default, will be set in initMapper()
  minimum_score_ = this->declare_parameter("minimumScore", 0.0);
  sigma_ = this->declare_parameter("sigma", 0.05);
  kernelSize_ = this->declare_parameter("kernelSize", 1);
  lstep_ = this->declare_parameter("lstep", 0.05);
  astep_ = this->declare_parameter("astep", 0.05);
  iterations_ = this->declare_parameter("iterations", 5);
  lsigma_ = this->declare_parameter("lsigma", 0.075);
  ogain_ = this->declare_parameter("ogain", 3.0);
  lskip_ = this->declare_parameter("lskip", 0);
  srr_ = this->declare_parameter("srr", 0.1);
  srt_ = this->declare_parameter("srt", 0.2);
  str_ = this->declare_parameter("str", 0.1);
  stt_ = this->declare_parameter("stt", 0.2);
  linearUpdate_ = this->declare_parameter("linearUpdate", 1.0);
  angularUpdate_ = this->declare_parameter("angularUpdate", 0.5);
  temporalUpdate_ = this->declare_parameter("temporalUpdate", -1.0);
  resampleThreshold_ = this->declare_parameter("resampleThreshold", 0.5);
  particles_ = this->declare_parameter("particles", 30);
  xmin_ = this->declare_parameter("xmin", -100.0);
  ymin_ = this->declare_parameter("ymin", -100.0);
  xmax_ = this->declare_parameter("xmax", 100.0);
  ymax_ = this->declare_parameter("ymax", 100.0);
  delta_ = this->declare_parameter("delta", 0.05);
  occ_thresh_ = this->declare_parameter("occ_thresh", 0.25);
  llsamplerange_ = this->declare_parameter("llsamplerange", 0.01);
  lasamplerange_ = this->declare_parameter("lasamplerange", 0.005);
  lasamplestep_ = this->declare_parameter("lasamplestep", 0.005);
  tf_delay_ = this->declare_parameter("tf_delay", transform_publish_period_);
}

void SlamGMapping::startLiveSlam()
{
  /* create publishers */
  entropy_publisher_ = this->create_publisher<std_msgs::msg::Float64>("entropy", 1);
  sst_ = this->create_publisher<nav_msgs::msg::OccupancyGrid>("map", 1);
  sstm_ = this->create_publisher<nav_msgs::msg::MapMetaData>("map_metadata", 1);
  /* create services */
  ss_ = this->create_service<nav_msgs::srv::GetMap>(
    "dynamic_map",
    std::bind(&SlamGMapping::mapCallback, this, std::placeholders::_1, std::placeholders::_2));
  /* create subscribers */
  rclcpp::QoS qos{rclcpp::QoS(1).durability_volatile()};
  scan_filter_sub_ = this->create_subscription<sensor_msgs::msg::LaserScan>(
    "scan", qos, std::bind(&SlamGMapping::laserCallback, this, std::placeholders::_1));

  /*
   * TODO(allenh1): re-enable message filters
   */

  // scan_filter_ = new tf::MessageFilter<sensor_msgs::LaserScan>(*scan_filter_sub_, tf_, odom_frame_, 5);
  // scan_filter_->registerCallback(std::bind(&SlamGMapping::laserCallback, this, std::placeholders::_1));
  /* create the transform thread */
  auto converted =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(transform_publish_period_));
  m_timer = this->create_wall_timer(converted, std::bind(&SlamGMapping::publishTransform, this));
}

/*
 * TODO(allenh1): replay from rosbag. We don't have rosbag yet.
 */
void SlamGMapping::startReplay(const std::string & bag_fname, std::string scan_topic)
{
  if (!bag_fname.size() || !scan_topic.size()) { /* Shut up, GCC */
    RCLCPP_ERROR(this->get_logger(), "bag name or scan_topic cannot be empty!");
  }
  throw std::runtime_error("The SlamGMapping::startReplay function has not been implemented yet");
//   double transform_publish_period;
//   ros::NodeHandle private_nh_("~");
//   entropy_publisher_ = private_nh_.advertise<std_msgs::Float64>("entropy", 1, true);
//   sst_ = node_.advertise<nav_msgs::OccupancyGrid>("map", 1, true);
//   sstm_ = node_.advertise<nav_msgs::MapMetaData>("map_metadata", 1, true);
//   ss_ = node_.advertiseService("dynamic_map", &SlamGMapping::mapCallback, this);

//   rosbag::Bag bag;
//   bag.open(bag_fname, rosbag::bagmode::Read);

//   std::vector<std::string> topics;
//   topics.push_back(std::string("/tf"));
//   topics.push_back(scan_topic);
//   rosbag::View viewall(bag, rosbag::TopicQuery(topics));

//   // Store up to 5 messages and there error message (if they cannot be processed right away)
//   std::queue<std::pair<sensor_msgs::LaserScan::ConstPtr, std::string> > s_queue;
//   for (rosbag::MessageInstance const m : viewall)
//   {
//     tf::tfMessage::ConstPtr cur_tf = m.instantiate<tf::tfMessage>();
//     if (cur_tf != NULL) {
//       for (size_t i = 0; i < cur_tf->transforms.size(); ++i)
//       {
//         geometry_msgs::TransformStamped transformStamped;
//         tf::StampedTransform stampedTf;
//         transformStamped = cur_tf->transforms[i];
//         tf::transformStampedMsgToTF(transformStamped, stampedTf);
//         tf_.setTransform(stampedTf);
//       }
//     }

//     sensor_msgs::LaserScan::ConstPtr s = m.instantiate<sensor_msgs::LaserScan>();
//     if (s != NULL) {
//       if (!(ros::Time(s->header.stamp)).is_zero())
//       {
//         s_queue.push(std::make_pair(s, ""));
//       }
//       // Just like in live processing, only process the latest 5 scans
//       if (s_queue.size() > 5) {
//         ROS_WARN_STREAM("Dropping old scan: " << s_queue.front().second);
//         s_queue.pop();
//       }
//       // ignoring un-timestamped tf data
//     }

//     // Only process a scan if it has tf data
//     while (!s_queue.empty())
//     {
//       try
//       {
//         tf::StampedTransform t;
//         tf_.lookupTransform(s_queue.front().first->header.frame_id, odom_frame_, s_queue.front().first->header.stamp, t);
//         this->laserCallback(s_queue.front().first);
//         s_queue.pop();
//       }
//       // If tf does not have the data yet
//       catch(tf2::TransformException& e)
//       {
//         // Store the error to display it if we cannot process the data after some time
//         s_queue.front().second = std::string(e.what());
//         break;
//       }
//     }
//   }

//   bag.close();
}

void
SlamGMapping::publishLoop(double transform_publish_period)
{
  if (transform_publish_period == 0) {
    RCLCPP_WARN(this->get_logger(), "transform_publish_period set to zero!");
    return;
  }

  rclcpp::Rate r{1.0 / transform_publish_period};
  while (rclcpp::ok()) {
    publishTransform();
    r.sleep();
  }
}

bool
SlamGMapping::getOdomPose(GMapping::OrientedPoint & gmap_pose, const auto & t)
{
  // Get the pose of the centered laser at the right time
  tf2::TimePoint tp = tf2::TimePoint(
    std::chrono::seconds(t.sec) + std::chrono::nanoseconds(t.nanosec));
  centered_laser_pose_.stamp_ = tp;
  // Get the laser's pose that is centered
  tf2::Stamped<tf2::Transform> odom_pose;
  geometry_msgs::msg::TransformStamped odom_pose_msg;
  try {
    buffer->transform(
      tf2::toMsg(centered_laser_pose_),
      odom_pose_msg,
      odom_frame_,
      tf2::durationFromSec(0.4)
    );
    tf2::fromMsg(odom_pose_msg, odom_pose);
  } catch (tf2::TransformException & e) {
    RCLCPP_WARN(this->get_logger(), "Failed to compute odom pose, skipping scan (%s)\n", e.what());
    std::vector<std::string> frames;
    frames.push_back("base_scan");
    frames.push_back("base_link");
    frames.push_back("base_footprint");
    frames.push_back("odom");

    /* can't transform odom --> base_scan, try intermediate */
    for (auto from_frame : frames) {
      for (auto to_frame : frames) {
        try {
          geometry_msgs::msg::TransformStamped odom_pose_msg;
          buffer->lookupTransform(from_frame, to_frame, tf2_ros::fromMsg(t));
          std::cerr << std::endl << "Transform from '" << from_frame <<
            "' to frame '" << to_frame << "' succeeded." << std::endl;
        } catch (tf2::TransformException & e) {
          std::cerr << std::endl << "Transform from '" << from_frame <<
            "' to frame '" << to_frame << "' failed: (" << e.what() << ")" << std::endl;
        }
      }
    }
    std::cerr << std::endl;
    return false;
  }

  double yaw, pitch, roll;
  odom_pose.getBasis().getEulerYPR(yaw, pitch, roll);
  gmap_pose = GMapping::OrientedPoint(
    odom_pose.getOrigin().x(), odom_pose.getOrigin().y(), yaw);
  return true;
}

bool
SlamGMapping::initMapper(const std::shared_ptr<sensor_msgs::msg::LaserScan> scan)
{
  laser_frame_ = scan->header.frame_id;
  // Get the laser's pose, relative to base.
  /* get a staped identity quaternion */
  tf2::Stamped<tf2::Transform> ident(
    tf2::Transform(
      tf2::Quaternion::getIdentity(),
      tf2::Vector3(0, 0, 0)),
    tf2_ros::fromMsg(scan->header.stamp),
    laser_frame_);
  tf2::Stamped<tf2::Transform> laser_pose;
  try {
    geometry_msgs::msg::TransformStamped laser_pose_msg;
    buffer->transform(
      tf2::toMsg(ident),
      laser_pose_msg,
      base_frame_,
      tf2::durationFromSec(0.4)
    );
    tf2::fromMsg(laser_pose_msg, laser_pose);
  } catch (tf2::TransformException & e) {
    RCLCPP_WARN(this->get_logger(), "Failed to compute laser pose, aborting initialization (%s)\n",
      e.what());
    return false;
  }

  // create a point 1m above the laser position and transform it into the laser-frame
  geometry_msgs::msg::PointStamped up;
  up.header.stamp = scan->header.stamp;
  up.header.frame_id = base_frame_;
  up.point.x = up.point.y = 0;
  up.point.z = 1 + laser_pose.getOrigin().z();

  try {
    buffer->transform(up, up, laser_frame_);
    RCLCPP_DEBUG(this->get_logger(), "Z-Axis in sensor frame: %.3f\n", up.point.z);
  } catch (tf2::TransformException & e) {
    RCLCPP_WARN(this->get_logger(), "Unable to determine orientation of laser: %s\n",
      e.what());
    return false;
  }

  // gmapping doesnt take roll or pitch into account. So check for correct sensor alignment.
  if (std::fabs(std::fabs(up.point.z) - 1) > 0.001) {
    RCLCPP_WARN(this->get_logger(), 
      "Laser has to be mounted planar! Z-coordinate has to be 1 or -1, but gave: %.5f\n",
      up.point.z);
    return false;
  }

  gsp_laser_beam_count_ = scan->ranges.size();

  double angle_center = (scan->angle_min + scan->angle_max) / 2;

  if (up.point.z > 0) {
    do_reverse_range_ = scan->angle_min > scan->angle_max;
    tf2::Quaternion q;
    q.setRPY(0.0, 0.0, angle_center);
    auto time_stamp = tf2_ros::fromMsg(this->now());
    centered_laser_pose_ = tf2::Stamped<tf2::Transform>(
      tf2::Transform(q, tf2::Vector3(0, 0, 0)), time_stamp, laser_frame_);
    RCLCPP_INFO(this->get_logger(), "Laser is mounted upwards.\n");
  } else {
    tf2::Quaternion q;
    q.setRPY(M_PI, 0, -angle_center);
    do_reverse_range_ = scan->angle_min < scan->angle_max;
    auto time_stamp = tf2_ros::fromMsg(this->now());
    centered_laser_pose_ = tf2::Stamped<tf2::Transform>(
      tf2::Transform(q, tf2::Vector3(0, 0, 0)), time_stamp, laser_frame_);
    RCLCPP_INFO(this->get_logger(), "Laser is mounted upside down.\n");
  }

  // Compute the angles of the laser from -x to x, basically symmetric and in increasing order
  laser_angles_.resize(scan->ranges.size());
  // Make sure angles are started so that they are centered
  double theta = -std::fabs(scan->angle_min - scan->angle_max) / 2;
  /*
   * TODO(allenh1): is this loop useful? Why do wee keep these values?
   */
  for (unsigned int i = 0; i < scan->ranges.size(); ++i) {
    laser_angles_[i] = theta;
    theta += std::fabs(scan->angle_increment);
  }

  RCLCPP_INFO(this->get_logger(), "Laser angles in laser-frame: min: %.3f max: %.3f inc: %.3f\n",
    scan->angle_min, scan->angle_max, scan->angle_increment);
  RCLCPP_INFO(this->get_logger(), "Laser angles in top-down centered laser-frame: min: %.3f max: %.3f inc: %.3f\n",
    laser_angles_.front(), laser_angles_.back(), std::fabs(scan->angle_increment));

  GMapping::OrientedPoint gmap_pose(0, 0, 0);

  // setting maxRange and maxUrange here so we can set a reasonable default
  maxRange_ = this->declare_parameter("maxRange", scan->range_max - 0.01);
  maxUrange_ = this->declare_parameter("maxUrange", maxRange_);

  // The laser must be called "FLASER".
  // We pass in the absolute value of the computed angle increment, on the
  // assumption that GMapping requires a positive angle increment.  If the
  // actual increment is negative, we'll swap the order of ranges before
  // feeding each scan to GMapping.
  gsp_laser_ = new GMapping::RangeSensor("FLASER",
      gsp_laser_beam_count_,
      fabs(scan->angle_increment),
      gmap_pose,
      0.0,
      maxRange_);
  assert(gsp_laser_);

  GMapping::SensorMap smap;
  smap.insert(make_pair(gsp_laser_->getName(), gsp_laser_));
  gsp_->setSensorMap(smap);

  gsp_odom_ = std::make_unique<GMapping::OdometrySensor>(odom_frame_);

  /// @todo Expose setting an initial pose
  GMapping::OrientedPoint initialPose;

  if (!getOdomPose(initialPose, scan->header.stamp)) {
    RCLCPP_WARN(this->get_logger(), 
      "Unable to determine inital pose of laser! Starting point will be set to zero.\n");
    initialPose = GMapping::OrientedPoint(0.0, 0.0, 0.0);
  }

  gsp_->setMatchingParameters(maxUrange_, maxRange_, sigma_,
    kernelSize_, lstep_, astep_, iterations_,
    lsigma_, ogain_, lskip_);

  gsp_->setMotionModelParameters(srr_, srt_, str_, stt_);
  gsp_->setUpdateDistances(linearUpdate_, angularUpdate_, resampleThreshold_);
  gsp_->setUpdatePeriod(temporalUpdate_);
  gsp_->setgenerateMap(false);
  gsp_->init(particles_, xmin_, ymin_, xmax_, ymax_, delta_, initialPose);
  gsp_->setllsamplerange(llsamplerange_);
  gsp_->setllsamplestep(llsamplestep_);
  /// @todo Check these calls; in the gmapping gui, they use
  /// llsamplestep and llsamplerange intead of lasamplestep and
  /// lasamplerange.  It was probably a typo, but who knows.
  gsp_->setlasamplerange(lasamplerange_);
  gsp_->setlasamplestep(lasamplestep_);
  gsp_->setminimumScore(minimum_score_);

  // Call the sampling function once to set the seed.
  GMapping::sampleGaussian(1, seed_);

  RCLCPP_INFO(this->get_logger(), "Initialization complete\n");

  return true;
}

bool
SlamGMapping::addScan(
  const std::shared_ptr<sensor_msgs::msg::LaserScan> scan,
  GMapping::OrientedPoint & gmap_pose)
{
  if (!getOdomPose(gmap_pose, scan->header.stamp)) {
    RCLCPP_ERROR(this->get_logger(), "Error: getOdomPose failed!");
    return false;
  }

  if (scan->ranges.size() != gsp_laser_beam_count_) {
    RCLCPP_ERROR(this->get_logger(), "Error: scan->ranges.size() != gsp_laser_beam_count_!");
    return false;
  }

  // GMapping wants an array of doubles...
  size_t num_ranges = scan->ranges.size();
  double * ranges_double = new double[num_ranges];
  // If the angle increment is negative, we have to invert the order of the readings.
  if (do_reverse_range_) {
    RCLCPP_DEBUG(this->get_logger(), "Inverting scan\n");
    for (size_t i = 0; i < num_ranges; i++) {
      // Must filter out short readings, because the mapper won't
      ranges_double[i] = (scan->ranges[num_ranges - i - 1] < scan->range_min) ?
        scan->range_max :
        scan->ranges[num_ranges - i - 1];
    }
  } else {
    for (size_t i = 0; i < num_ranges; i++) {
      // Must filter out short readings, because the mapper won't
      ranges_double[i] = (scan->ranges[i] < scan->range_min) ?
        scan->range_max :
        scan->ranges[i];
    }
  }

  tf2::TimePoint stamp_time = tf2_ros::fromMsg(scan->header.stamp);

  GMapping::RangeReading reading(num_ranges,
    ranges_double,
    gsp_laser_,
    tf2::timeToSec(stamp_time));

  // ...but it deep copies them in RangeReading constructor, so we don't
  // need to keep our array around.
  delete[] ranges_double;

  reading.setPose(gmap_pose);

  RCLCPP_DEBUG(this->get_logger(), "scanpose (%.3f): %.3f %.3f %.3f\n",
    scan->header.stamp.sec +
    scan->header.stamp.nanosec * 1.0e-6,
    gmap_pose.x,
    gmap_pose.y,
    gmap_pose.theta);

  RCLCPP_DEBUG(this->get_logger(), "processing scan\n");
  bool ret = gsp_->processScan(reading);
  if (!ret) {
    /* Is this the value to check? Could it be true? */
    RCLCPP_ERROR(this->get_logger(), "gsp->processScan(reading); failed!");
  }
  return ret;
}

void
SlamGMapping::laserCallback(const std::shared_ptr<sensor_msgs::msg::LaserScan> scan)
{
  if (!buffer->canTransform(odom_frame_,
    scan->header.frame_id,
    tf2_ros::fromMsg(scan->header.stamp),
    tf2::durationFromSec(0.3)))
  {
    return;
  }

  laser_count_++;
  if ((laser_count_ % throttle_scans_) != 0) {
    return;
  }

  auto last_map_update = tf2::TimePointZero;

  // We can't initialize the mapper until we've got the first scan
  if (!got_first_scan_) {
    if (!initMapper(scan)) {
      std::cerr << "Mapper not initialized!" << std::endl;
      return;
    }
    got_first_scan_ = true;
  }

  GMapping::OrientedPoint odom_pose;

  if (addScan(scan, odom_pose)) {
    RCLCPP_DEBUG(this->get_logger(), "scan processed\n");

    GMapping::OrientedPoint mpose = gsp_->getParticles()[gsp_->getBestParticleIndex()].pose;
    RCLCPP_DEBUG(this->get_logger(), "new best pose: %.3f %.3f %.3f\n", mpose.x, mpose.y, mpose.theta);
    RCLCPP_DEBUG(this->get_logger(), "odom pose: %.3f %.3f %.3f\n", odom_pose.x, odom_pose.y, odom_pose.theta);
    RCLCPP_DEBUG(this->get_logger(), "correction: %.3f %.3f %.3f\n", mpose.x - odom_pose.x, mpose.y - odom_pose.y,
      mpose.theta - odom_pose.theta);

    tf2::Quaternion mpose_q, odom_q;
    mpose_q.setRPY(0.0, 0.0, mpose.theta);
    odom_q.setRPY(0.0, 0.0, odom_pose.theta);
    tf2::Transform laser_to_map =
      tf2::Transform(mpose_q, tf2::Vector3(mpose.x, mpose.y, 0.0)).inverse();
    tf2::Transform odom_to_laser =
      tf2::Transform(odom_q, tf2::Vector3(odom_pose.x, odom_pose.y, 0.0));

    map_to_odom_mutex_.lock();
    map_to_odom_ = (odom_to_laser * laser_to_map).inverse();
    map_to_odom_mutex_.unlock();

    tf2::TimePoint stamp_time = tf2_ros::fromMsg(scan->header.stamp);

    if (!got_map_ || (stamp_time - last_map_update) > map_update_interval_) {
      updateMap(scan);
      last_map_update = tf2_ros::fromMsg(scan->header.stamp);
      RCLCPP_DEBUG(this->get_logger(), "Updated the map\n");
    }
  } else {
    RCLCPP_DEBUG(this->get_logger(), "cannot process scan\n");
  }
}

double
SlamGMapping::computePoseEntropy()
{
  double weight_total = 0.0;
  for (const auto & it : gsp_->getParticles()) {
    weight_total += it.weight;
  }

  double entropy = 0.0;
  for (const auto & it : gsp_->getParticles()) {
    if ((it.weight / weight_total) > 0.0) {
      entropy += it.weight / weight_total * log(it.weight / weight_total);
    }
  }
  return -entropy;
}

void
SlamGMapping::updateMap(const std::shared_ptr<sensor_msgs::msg::LaserScan> scan)
{
  RCLCPP_DEBUG(this->get_logger(), "Update map\n");
  std::lock_guard<std::mutex> map_lock(map_mutex_);
  GMapping::ScanMatcher matcher;

  matcher.setLaserParameters(scan->ranges.size(), &(laser_angles_[0]),
    gsp_laser_->getPose());

  matcher.setlaserMaxRange(maxRange_);
  matcher.setusableRange(maxUrange_);
  matcher.setgenerateMap(true);

  GMapping::GridSlamProcessor::Particle best =
    gsp_->getParticles()[gsp_->getBestParticleIndex()];
  std_msgs::msg::Float64 entropy;
  entropy.data = computePoseEntropy();
  if (entropy.data > 0.0) {
    entropy_publisher_->publish(entropy);
  }

  if (!got_map_) {
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
  center.x = (xmin_ + xmax_) / 2.0;
  center.y = (ymin_ + ymax_) / 2.0;

  GMapping::ScanMatcherMap smap(center, xmin_, ymin_, xmax_, ymax_, delta_);

  RCLCPP_DEBUG(this->get_logger(), "Trajectory tree:\n");
  for (auto n = best.node; n; n = n->parent) {
    RCLCPP_DEBUG(this->get_logger(), "  %.3f %.3f %.3f\n",
      n->pose.x,
      n->pose.y,
      n->pose.theta);
    if (!n->reading) {
      RCLCPP_DEBUG(this->get_logger(), "Reading is NULL\n");
      continue;
    }
    matcher.invalidateActiveArea();
    matcher.computeActiveArea(smap, n->pose, &((*n->reading)[0]));
    matcher.registerScan(smap, n->pose, &((*n->reading)[0]));
  }

  // the map may have expanded, so resize ros message as well
  if (map_.map.info.width != (unsigned int) smap.getMapSizeX() ||
    map_.map.info.height != (unsigned int) smap.getMapSizeY())
  {

    // NOTE: The results of ScanMatcherMap::getSize() are different from the parameters given to the constructor
    //       so we must obtain the bounding box in a different way
    GMapping::Point wmin = smap.map2world(GMapping::IntPoint(0, 0));
    GMapping::Point wmax =
      smap.map2world(GMapping::IntPoint(smap.getMapSizeX(), smap.getMapSizeY()));
    xmin_ = wmin.x; ymin_ = wmin.y;
    xmax_ = wmax.x; ymax_ = wmax.y;

    RCLCPP_DEBUG(this->get_logger(), "map size is now %dx%d pixels (%f,%f)-(%f, %f)\n",
      smap.getMapSizeX(), smap.getMapSizeY(),
      xmin_, ymin_, xmax_, ymax_);

    map_.map.info.width = smap.getMapSizeX();
    map_.map.info.height = smap.getMapSizeY();
    map_.map.info.origin.position.x = xmin_;
    map_.map.info.origin.position.y = ymin_;
    map_.map.data.resize(map_.map.info.width * map_.map.info.height);

    RCLCPP_DEBUG(this->get_logger(), "map origin: (%f, %f)\n", map_.map.info.origin.position.x,
      map_.map.info.origin.position.y);
  }

  int map_size_x = smap.getMapSizeX();
  int map_size_y = smap.getMapSizeY();
  for (int x = 0; x < map_size_x; ++x) {
    for (int y = 0; y < map_size_y; ++y) {
      /// @todo Sort out the unknown vs. free vs. obstacle thresholding
      GMapping::IntPoint p(x, y);
      double occ = smap.cell(p);
      assert(occ <= 1.0);
      if (occ < 0) {
        map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = -1;
      } else if (occ > occ_thresh_) {
        //map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = (int)round(occ*100.0);
        map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = 100;
      } else {
        map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = 0;
      }
    }
  }
  got_map_ = true;

  //make sure to set the header information on the map
  map_.map.header.stamp = this->now();
  map_.map.header.frame_id = map_frame_;

  sst_->publish(map_.map);
  sstm_->publish(map_.map.info);
}

bool
SlamGMapping::mapCallback(
  const std::shared_ptr<nav_msgs::srv::GetMap::Request> req,
  std::shared_ptr<nav_msgs::srv::GetMap::Response> res)
{
  std::lock_guard<std::mutex> map_lock(map_mutex_);
  if (req != nullptr && got_map_ && map_.map.info.width && map_.map.info.height) {
    *res = map_;
    return true;
  }
  return false;
}

void SlamGMapping::publishTransform()
{
  map_to_odom_mutex_.lock();
  auto tf_expiration = tf2_ros::fromMsg(this->now()) + tf2::durationFromSec(tf_delay_);
  geometry_msgs::msg::TransformStamped tmp_tf_stamped;
  tmp_tf_stamped.header.frame_id = map_frame_;
  tmp_tf_stamped.child_frame_id = odom_frame_;
  tmp_tf_stamped.header.stamp = tf2_ros::toMsg(tf_expiration);
  tmp_tf_stamped.transform = tf2::toMsg(map_to_odom_);
  tfB_->sendTransform(tmp_tf_stamped);
  map_to_odom_mutex_.unlock();
}
