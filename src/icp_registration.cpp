#include <icp_registration.h>


IcpRegistration::IcpRegistration() :
  nh_private_("~"), original_target_(new PointCloudRGB), target_readed_(false),
  enable_(false), in_clouds_num_(0), last_detection_(ros::Time(-100)),
  robot2camera_init_(false) {
  // Read params
  nh_private_.param("min_range",        min_range_,       1.0);
  nh_private_.param("max_range",        max_range_,       2.5);
  nh_private_.param("voxel_size",       voxel_size_,      0.02);
  nh_private_.param("target",           target_file_,     std::string("target.pcd"));
  nh_private_.param("robot_frame_id",   robot_frame_id_,  std::string("robot"));
  nh_private_.param("world_frame_id",   world_frame_id_,  std::string("world"));
  nh_private_.param("target_frame_id",  target_frame_id_, std::string("target"));
  nh_private_.param("target_tf_topic",  target_tf_topic_, std::string("target"));
  nh_private_.param("remove_ground",    remove_ground_,   true);
  nh_private_.param("ground_height",    ground_height_,   0.09);
  nh_private_.param("max_icp_dist",     max_icp_dist_,    2.0);
  nh_private_.param("max_icp_score",    max_icp_score_,   0.0001);
  nh_private_.param("use_color",        use_color_,       true);
  nh_private_.param("reset_timeout",    reset_timeout_,   6.0);



  // Subscribers and publishers
  point_cloud_sub_ = nh_.subscribe(
    "input_cloud", 1, &IcpRegistration::pointCloudCb, this);
  dbg_reg_cloud_pub_ = nh_private_.advertise<sensor_msgs::PointCloud2>(
    "dbg_reg_cloud", 1);
  dbg_obj_cloud_pub_ = nh_private_.advertise<sensor_msgs::PointCloud2>(
    "dbg_obj_cloud", 1);
  target_pose_pub_ = nh_.advertise<geometry_msgs::Pose>(
    target_tf_topic_, 1);

  // Services
  enable_srv_ = nh_private_.advertiseService("enable",
    &IcpRegistration::enable, this);
  disable_srv_ = nh_private_.advertiseService("disable",
    &IcpRegistration::disable, this);
}

void IcpRegistration::pointCloudCb(const sensor_msgs::PointCloud2::ConstPtr& in_cloud) {
  if (!target_readed_) {
    ROS_INFO_STREAM("[IcpRegistration]: Loading target for the first time...");

    // Opening target
    if (pcl::io::loadPCDFile<PointRGB>(target_file_, *original_target_) == -1) {
      ROS_ERROR_STREAM("[IcpRegistration]: Couldn't read file " <<
        target_file_);
      return;
    }

    // Filter target
    filter(original_target_);
    target_readed_ = true;
  }

  if (!enable_) {
    ROS_INFO_THROTTLE(15, "[IcpRegistration]: Not enabled.");
    return;
  }

  // Copy
  PointCloudRGB::Ptr original(new PointCloudRGB);
  PointCloudRGB::Ptr cloud(new PointCloudRGB);
  fromROSMsg(*in_cloud, *cloud);
  fromROSMsg(*in_cloud, *original);

  if (cloud->points.size() < 100) {
    ROS_WARN("[IcpRegistration]: Input cloud has less than 100 points.");
    return;
  }

  // Translate the cloud to the robot frame id to remove the camera orientation effect
  if (!robot2camera_init_) {
    bool ok = getRobot2Camera(in_cloud->header.frame_id);
    if (!ok) return;
  }
  move(cloud, robot2camera_);

  // Filter input cloud
  filter(cloud, true, true);

  if (cloud->points.size() < 100) {
    ROS_WARN("[IcpRegistration]: Input cloud has not enough points after filtering.");
    return;
  }

  // Remove ground
  if (remove_ground_)
    removeGround(cloud, in_cloud->header.stamp);

  if (cloud->points.size() < 100) {
    ROS_WARN("[IcpRegistration]: Input cloud has not enough points after ground filtering.");
    return;
  }

  // Move target
  PointCloudRGB::Ptr target(new PointCloudRGB);
  pcl::copyPointCloud(*original_target_, *target);
  double elapsed = fabs(last_detection_.toSec() - ros::Time::now().toSec());
  if (elapsed > reset_timeout_) {
    // Move to center
    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*cloud, centroid);
    tf::Transform tf_01;
    tf_01.setIdentity();
    tf_01.setOrigin(tf::Vector3(centroid[0], centroid[1], centroid[2]));
    move(target, tf_01);
    last_pose_ = tf_01;
    first_iter_ = true;
  } else {
    // Move to last detected position
    move(target, last_pose_);
    first_iter_ = false;
  }

  // Registration
  bool converged;
  double score;
  tf::Transform target_pose;
  pairAlign(target, cloud, target_pose, converged, score);

  double dist = eucl(last_pose_, target_pose);
  if (converged) {
    ROS_INFO_STREAM("[IcpRegistration]: Icp converged. Score: " <<
      score << ". Dist: " << dist);
  }
  if (converged && dist < max_icp_dist_ && score < max_icp_score_) {
    ROS_INFO_STREAM("[IcpRegistration]: Target found with score of " <<
      score);

    double roll, pitch, yaw;
    tf::Matrix3x3((target_pose * last_pose_).getRotation()).getRPY(roll, pitch, yaw);
    if(!first_iter_) {
      while(yaw < 0.0) yaw += 2*M_PI;
      while(yaw > 2*M_PI) yaw -= 2*M_PI;
      double diff;
      if (yaw > last_yaw_) {
        diff = yaw-last_yaw_;
      } else {
        diff = last_yaw_-yaw;
      }
      while(diff < 0.0) diff += 2*M_PI;
      while(diff > 2*M_PI) diff -= 2*M_PI;
      ROS_INFO_STREAM("[IcpRegistration]: Angle diff: " << diff*180/M_PI << "dg.");
      if (diff > 20*M_PI/180 && diff < 340*M_PI/180) return;
    }
    last_yaw_ = yaw;

    // Update
    last_pose_ = target_pose * last_pose_;
    last_detection_ = ros::Time::now();

    // Publish tf and message
    publish(last_pose_, in_cloud->header.stamp);

    // Debug cloud
    if (dbg_reg_cloud_pub_.getNumSubscribers() > 0) {
      move(original, robot2camera_);
      PointCloudRGB::Ptr dbg_cloud(new PointCloudRGB);
      pcl::copyPointCloud(*original, *dbg_cloud);

      // Add target to debug cloud
      move(target, target_pose);

      PointCloudRGB::Ptr target_color(new PointCloudRGB);
      for (uint i=0; i < target->size(); i++) {
        PointRGB prgb(0, 255, 0);
        prgb.x = target->points[i].x;
        prgb.y = target->points[i].y;
        prgb.z = target->points[i].z;
        target_color->push_back(prgb);
      }
      *dbg_cloud += *target_color;

      // Publish
      sensor_msgs::PointCloud2 dbg_cloud_ros;
      toROSMsg(*dbg_cloud, dbg_cloud_ros);
      dbg_cloud_ros.header.stamp = in_cloud->header.stamp;
      dbg_cloud_ros.header.frame_id = robot_frame_id_;
      dbg_reg_cloud_pub_.publish(dbg_cloud_ros);
    }
  } else {
    ROS_WARN_STREAM("[IcpRegistration]: Target not found in the input " <<
      "pointcloud. Trying again...");
  }

  // Publish debug cloud
  if (dbg_reg_cloud_pub_.getNumSubscribers() > 0) {
    move(original, robot2camera_);
    PointCloudRGB::Ptr dbg_cloud(new PointCloudRGB);
    pcl::copyPointCloud(*original, *dbg_cloud);

    // Publish
    sensor_msgs::PointCloud2 dbg_cloud_ros;
    toROSMsg(*dbg_cloud, dbg_cloud_ros);
    dbg_cloud_ros.header.stamp = in_cloud->header.stamp;
    dbg_cloud_ros.header.frame_id = robot_frame_id_;
    dbg_reg_cloud_pub_.publish(dbg_cloud_ros);
  }
}

void IcpRegistration::pairAlign(PointCloudRGB::Ptr src,
                                PointCloudRGB::Ptr tgt,
                                tf::Transform &output,
                                bool &converged,
                                double &score) {
  ROS_INFO_STREAM("[IcpRegistration]: Target pointcloud " << src->size() <<
    " points. Scene pointcloud " << tgt->size() << " points.");
  // Remove color from pointcloud
  PointCloud::Ptr src_no_color(new PointCloud);
  PointCloud::Ptr tgt_no_color(new PointCloud);
  pcl::copyPointCloud(*src, *src_no_color);
  pcl::copyPointCloud(*tgt, *tgt_no_color);

  // Align
  PointCloud::Ptr aligned(new PointCloud);
  IterativeClosestPoint icp;
  icp.setMaxCorrespondenceDistance(0.07);
  icp.setRANSACOutlierRejectionThreshold(0.001);
  icp.setTransformationEpsilon(0.00001);
  icp.setEuclideanFitnessEpsilon(0.001);
  icp.setMaximumIterations(100);
  icp.setInputSource(src_no_color);
  icp.setInputTarget(tgt_no_color);
  icp.align(*aligned);

  // The transform
  output = matrix4fToTf(icp.getFinalTransformation());

  // The measures
  converged = icp.hasConverged();
  score = icp.getFitnessScore();
}

void IcpRegistration::filter(PointCloudRGB::Ptr cloud,
                             const bool& passthrough,
                             const bool& statistical) {
  std::vector<int> indicies;
  pcl::removeNaNFromPointCloud(*cloud, *cloud, indicies);

  if (passthrough) {
    pcl::PassThrough<PointRGB> pass;
    pass.setFilterFieldName("z");
    pass.setFilterLimits(min_range_, max_range_);
    pass.setInputCloud(cloud);
    pass.filter(*cloud);
  }

  pcl::ApproximateVoxelGrid<PointRGB> grid;
  grid.setLeafSize(voxel_size_, voxel_size_, voxel_size_);
  grid.setDownsampleAllData(true);
  grid.setInputCloud(cloud);
  grid.filter(*cloud);

  if (statistical) {
    pcl::RadiusOutlierRemoval<PointRGB> outrem;
    outrem.setInputCloud(cloud);
    outrem.setRadiusSearch(0.2);
    outrem.setMinNeighborsInRadius(100);
    outrem.filter(*cloud);
  }
}

void IcpRegistration::removeGround(PointCloudRGB::Ptr cloud,
                                   const ros::Time& stamp) {
  pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);
  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);

  // Will depend on the use of color
  if (!use_color_) {
    PointCloud::Ptr cloud_no_color(new PointCloud);
    pcl::copyPointCloud(*cloud, *cloud_no_color);
    pcl::SACSegmentation<Point> seg;
    seg.setModelType(pcl::SACMODEL_PLANE);
    seg.setMethodType(pcl::SAC_RANSAC);
    seg.setDistanceThreshold(ground_height_);
    seg.setInputCloud(cloud_no_color);
    seg.segment(*inliers, *coefficients);
  } else {
    pcl::SACSegmentation<PointRGB> seg;
    seg.setModelType(pcl::SACMODEL_PLANE);
    seg.setMethodType(pcl::SAC_RANSAC);
    seg.setDistanceThreshold(ground_height_);
    seg.setInputCloud(cloud);
    seg.segment(*inliers, *coefficients);
  }

  double mean_z;
  PointCloudRGB::Ptr ground(new PointCloudRGB);
  PointCloudRGB::Ptr objects(new PointCloudRGB);
  for (int i=0; i < (int)cloud->size(); i++) {
    if (std::find(inliers->indices.begin(), inliers->indices.end(), i) == inliers->indices.end()) {
      objects->push_back(cloud->points[i]);
    } else {
      ground->push_back(cloud->points[i]);
      mean_z += cloud->points[i].z;
    }
  }
  mean_z = mean_z / ground->size();

  // Filter outliers in the objects
  PointCloudRGB::Ptr object_inliers(new PointCloudRGB);
  for (uint i=0; i < objects->size(); i++) {
    if ( fabs(objects->points[i].z - mean_z) < 0.35)
      object_inliers->push_back(objects->points[i]);
  }

  pcl::copyPointCloud(*object_inliers, *cloud);

  if (dbg_obj_cloud_pub_.getNumSubscribers() > 0) {
    for (uint i=0; i < object_inliers->size(); i++) {
      object_inliers->points[i].r = 255;
      object_inliers->points[i].g = 0;
      object_inliers->points[i].b = 0;
    }
    *ground += *object_inliers;
    sensor_msgs::PointCloud2 dbg_cloud_ros;
    toROSMsg(*ground, dbg_cloud_ros);
    dbg_cloud_ros.header.stamp = stamp;
    dbg_cloud_ros.header.frame_id = robot_frame_id_;
    dbg_obj_cloud_pub_.publish(dbg_cloud_ros);
  }
}

void IcpRegistration::publish(const tf::Transform& robot_to_target_in,
                              const ros::Time& stamp) {

  tf::Transform robot_to_target = robot_to_target_in;
  tf::Matrix3x3 rot = robot_to_target.getBasis();
  double r, p, y;
  rot.getRPY(r, p, y);
  tf::Quaternion q;
  q.setRPY(0.0, 0.0, y);
  robot_to_target.setRotation(q);

  // Publish tf
  tf_broadcaster_.sendTransform(
    tf::StampedTransform(robot_to_target,
                         stamp,
                         robot_frame_id_,
                         target_frame_id_));

  // Publish geometry message from world frame id
  if (target_pose_pub_.getNumSubscribers() > 0) {
    try {
      ros::Time now = ros::Time::now();
      tf::StampedTransform world2robot;
      tf_listener_.waitForTransform(world_frame_id_,
                                    robot_frame_id_,
                                    now, ros::Duration(1.0));
      tf_listener_.lookupTransform(world_frame_id_,
          robot_frame_id_, now, world2robot);

      // Compose the message
      geometry_msgs::Pose pose_msg;
      tf::Transform world2target = world2robot * robot_to_target;
      pose_msg.position.x = world2target.getOrigin().x();
      pose_msg.position.y = world2target.getOrigin().y();
      pose_msg.position.z = world2target.getOrigin().z();
      pose_msg.orientation.x = world2target.getRotation().x();
      pose_msg.orientation.y = world2target.getRotation().y();
      pose_msg.orientation.z = world2target.getRotation().z();
      pose_msg.orientation.w = world2target.getRotation().w();
      target_pose_pub_.publish(pose_msg);
    } catch (tf::TransformException ex) {
      ROS_WARN_STREAM("[IcpRegistration]: Cannot find the tf between " <<
        "world frame id and camera. " << ex.what());
    }
  }
}

tf::Transform IcpRegistration::matrix4fToTf(const Eigen::Matrix4f& in) {
  tf::Vector3 t_out;
  t_out.setValue(static_cast<double>(in(0,3)),
                 static_cast<double>(in(1,3)),
                 static_cast<double>(in(2,3)));

  tf::Matrix3x3 tf3d;
  tf3d.setValue(static_cast<double>(in(0,0)), static_cast<double>(in(0,1)), static_cast<double>(in(0,2)),
                static_cast<double>(in(1,0)), static_cast<double>(in(1,1)), static_cast<double>(in(1,2)),
                static_cast<double>(in(2,0)), static_cast<double>(in(2,1)), static_cast<double>(in(2,2)));

  tf::Quaternion q_out;
  tf3d.getRotation(q_out);
  tf::Transform out(q_out, t_out);
  return out;
}

void IcpRegistration::move(const PointCloudRGB::Ptr& cloud,
                           const tf::Transform& trans) {
  Eigen::Affine3d trans_eigen;
  transformTFToEigen(trans, trans_eigen);
  pcl::transformPointCloud(*cloud, *cloud, trans_eigen);
}

double IcpRegistration::eucl(const tf::Transform& a, const tf::Transform& b) {
  return sqrt( (a.getOrigin().x() - b.getOrigin().x())*(a.getOrigin().x() - b.getOrigin().x()) +
               (a.getOrigin().y() - b.getOrigin().y())*(a.getOrigin().y() - b.getOrigin().y()) +
               (a.getOrigin().z() - b.getOrigin().z())*(a.getOrigin().z() - b.getOrigin().z()) );
}

bool IcpRegistration::getRobot2Camera(const std::string& camera_frame_id) {
  try {
    ros::Time now = ros::Time::now();
    tf_listener_.waitForTransform(robot_frame_id_,
                                  camera_frame_id,
                                  now, ros::Duration(1.0));
    tf_listener_.lookupTransform(robot_frame_id_,
        camera_frame_id, now, robot2camera_);
    robot2camera_init_ = true;
    return true;
  } catch (tf::TransformException ex) {
    ROS_WARN_STREAM("[IcpRegistration]: Cannot find the tf between " <<
      "robot frame id and camera. " << ex.what());
    return false;
  }
}

bool IcpRegistration::enable(std_srvs::Empty::Request& req,
                             std_srvs::Empty::Response& res) {
  ROS_INFO("[IcpRegistration]: Enabled!");
  enable_ = true;
}
bool IcpRegistration::disable(std_srvs::Empty::Request& req,
                              std_srvs::Empty::Response& res) {
  ROS_INFO("[IcpRegistration]: Disabled!");
  enable_ = false;
}
