#ifndef ICP_REGISTRATION_H
#define ICP_REGISTRATION_H

#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <std_srvs/Empty.h>

#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl_conversions/pcl_conversions.h>

#include <pcl/point_types.h>
#include <pcl/common/common.h>
#include <pcl/registration/icp.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>

#include <string>
#include <boost/lexical_cast.hpp>

typedef pcl::PointXYZ Point;
typedef pcl::PointXYZRGB PointRGB;
typedef pcl::PointCloud<Point> PointCloud;
typedef pcl::PointCloud<PointRGB> PointCloudRGB;
typedef pcl::IterativeClosestPoint<Point, Point> IterativeClosestPoint;

class IcpRegistration {
 public:
  IcpRegistration();

 protected:
  void pointCloudCb(const sensor_msgs::PointCloud2::ConstPtr& in_cloud);

  void pairAlign(PointCloudRGB::Ptr src,
                 PointCloudRGB::Ptr tgt,
                 tf::Transform &output,
                 bool &converged,
                 double &score);

  void filter(PointCloudRGB::Ptr cloud,
              const bool& passthrough = false,
              const bool& statistical = false);

  void removeGround(PointCloudRGB::Ptr cloud, const ros::Time& stamp);

  void publish(const tf::Transform& robot_to_target,
               const ros::Time& stamp);

  bool getRobot2Camera(const std::string& camera_frame_id);

  tf::Transform matrix4fToTf(const Eigen::Matrix4f& in);

  void move(const PointCloudRGB::Ptr& cloud, const tf::Transform& trans);

  double eucl(const tf::Transform& a, const tf::Transform& b);

  bool enable(std_srvs::Empty::Request&  req,
              std_srvs::Empty::Response& res);

  bool disable(std_srvs::Empty::Request&  req,
               std_srvs::Empty::Response& res);

 private:
  ros::NodeHandle nh_;
  ros::NodeHandle nh_private_;

  ros::Subscriber point_cloud_sub_;
  ros::Publisher dbg_reg_cloud_pub_;
  ros::Publisher dbg_obj_cloud_pub_;
  ros::Publisher target_pose_pub_;
  ros::Publisher dbg_target_pose_pub_;
  ros::ServiceServer enable_srv_;
  ros::ServiceServer disable_srv_;
  tf::TransformBroadcaster tf_broadcaster_;
  tf::TransformListener tf_listener_;
  tf::StampedTransform robot2camera_;

  // Params
  double min_range_;
  double max_range_;
  double voxel_size_;
  std::string target_file_;
  std::string target_frame_id_;
  std::string robot_frame_id_;
  std::string world_frame_id_;
  std::string target_tf_topic_;
  bool remove_ground_;
  double ground_height_;
  double max_icp_dist_;
  double max_icp_score_;
  bool use_color_;

  // Operational variables
  PointCloudRGB::Ptr original_target_;
  bool target_readed_;
  bool enable_;
  int in_clouds_num_;
  tf::Transform last_pose_;
  ros::Time last_detection_;
  bool robot2camera_init_;

};

#endif // ICP_REGISTRATION_H
