/**
 * @file IcpMethodCali.cpp
 * @author huizeyu (huizeyu@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2024-04-18
 *
 * @copyright Copyright (c) 2024
 *
 */

#include "IcpMethodCali.h"

#include <pcl/kdtree/kdtree_flann.h>

#include <algorithm>
#include <pcl/impl/point_types.hpp>
#include <string>

#include "manager/DataManager.h"
#include "paramLoad/LivoxConfig.h"
#include "pcl/common/eigen.h"

namespace calibrate {
IcpMethodCali::IcpMethodCali() {
  auto lidar_info_vec = paramLoad::LivoxConfig::GetInstance()->m_livoxInfoVec;
  m_lastResult.x = lidar_info_vec[1].x;
  m_lastResult.y = lidar_info_vec[1].y;
  m_lastResult.z = lidar_info_vec[1].z;
  m_lastResult.roll = lidar_info_vec[1].roll * M_PI / 180.0f;
  m_lastResult.pitch = lidar_info_vec[1].pitch * M_PI / 180.0f;
  m_lastResult.yaw = lidar_info_vec[1].yaw * M_PI / 180.0f;
  LOG(WARNING) << "INIT GUSS = " << m_lastResult.ShowResult();
}

IcpMethodCali::~IcpMethodCali() {
}

Result IcpMethodCali::Exectute() {
  Result r;

  // 加载源点云和目标点云
  pcl::PointCloud<pcl::PointXYZI>::Ptr source_cloud(
      new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr target_cloud(
      new pcl::PointCloud<pcl::PointXYZI>);

  SyncCloud(source_cloud, target_cloud);

  if (source_cloud->empty() || target_cloud->empty()) {
    return r;
  }

  FindCoView(source_cloud, target_cloud);

  source_cloud = TransformPointCloud(source_cloud, m_lastResult);

  // 创建ICP对象
  pcl::IterativeClosestPoint<pcl::PointXYZI, pcl::PointXYZI> icp;
  icp.setInputSource(source_cloud);
  icp.setInputTarget(target_cloud);

  // 设置ICP参数
  icp.setMaxCorrespondenceDistance(0.1);  // 最大对应距离
  icp.setMaximumIterations(200);          // 最大迭代次数
  icp.setTransformationEpsilon(1e-6);     // 变换阈值
  icp.setEuclideanFitnessEpsilon(0.01);   // 拟合误差阈值
  icp.setRANSACIterations(0);

  // 执行配准
  pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_source(
      new pcl::PointCloud<pcl::PointXYZI>);
  icp.align(*transformed_source);

  static int times = 0;

  if (icp.hasConverged()) {
    Eigen::Affine3f correctionLidarFrame;
    correctionLidarFrame = icp.getFinalTransformation();
    LOG(INFO) << "average distance = " << icp.getFitnessScore();
    pcl::getTranslationAndEulerAngles(correctionLidarFrame, r.x, r.y, r.z,
                                      r.roll, r.pitch, r.yaw);

    if (paramLoad::LivoxConfig::GetInstance()->save_pcd) {
      std::string save_times_pcd = m_saveCaliedPcdPath + std::to_string(times);
      std::string command = "mkdir -p " + save_times_pcd;
      system(command.c_str());
      pcl::io::savePCDFileASCII(
          save_times_pcd + "/" + std::to_string(times) + "_source.pcd",
          *source_cloud);
      pcl::io::savePCDFileASCII(
          save_times_pcd + "/" + std::to_string(times) + "_target.pcd",
          *target_cloud);
      *transformed_source = *target_cloud + *transformed_source;
      pcl::io::savePCDFileASCII(
          save_times_pcd + "/" + std::to_string(times) + ".pcd",
          *transformed_source);
      times += 1;
    }
    r.x = m_lastResult.x + r.x;
    r.y = m_lastResult.y + r.y;
    r.z = m_lastResult.z + r.z;
    r.roll = m_lastResult.roll + r.roll;
    r.pitch = m_lastResult.pitch + r.pitch;
    r.yaw = m_lastResult.yaw + r.yaw;
    m_lastResult = r;
    m_resultsVec.push_back(r);
  } else {
    LOG(WARNING) << "The point cloud calculation failed for this frame";
  }
  return r;
}

void IcpMethodCali::SyncCloud(
    pcl::PointCloud<pcl::PointXYZI>::Ptr source_cloud,
    pcl::PointCloud<pcl::PointXYZI>::Ptr target_cloud) {
  auto data = manager::DataManager::GetInstance();
  livox_ros_driver2::msg::CustomMsg::SharedPtr tmp_target = nullptr;
  livox_ros_driver2::msg::CustomMsg::SharedPtr tmp_source = nullptr;
  tmp_target = data->GetFrontMsg(0);
  tmp_source = data->GetFrontMsg(1);
  if (tmp_target == nullptr || tmp_source == nullptr) {
    return;
  }
  bool is_sync = false;
  while (is_sync) {
    if (CheckHeader(tmp_target->header, tmp_source->header) == 1) {
      LOG(WARNING) << "wait sync id:1";
      tmp_source = data->GetFrontMsg(1);
    } else if (CheckHeader(tmp_target->header, tmp_source->header) == 0) {
      LOG(WARNING) << "wait sync id:0";
      tmp_target = data->GetFrontMsg(0);
    } else if (CheckHeader(tmp_target->header, tmp_source->header) == 100) {
      is_sync = true;
    }
  }
  std::thread t0 = std::thread([&]() {
    for (size_t i = 0; i < tmp_target->points.size(); i++) {
      pcl::PointXYZI p;
      p.x = tmp_target->points[i].x;
      p.y = tmp_target->points[i].y;
      p.z = tmp_target->points[i].z;
      p.intensity = tmp_target->points[i].reflectivity;
      target_cloud->push_back(p);
    }
  });
  std::thread t1 = std::thread([&]() {
    for (size_t i = 0; i < tmp_source->points.size(); i++) {
      pcl::PointXYZI p;
      p.x = tmp_source->points[i].x;
      p.y = tmp_source->points[i].y;
      p.z = tmp_source->points[i].z;
      p.intensity = tmp_source->points[i].reflectivity;
      source_cloud->push_back(p);
    }
  });
  t0.join();
  t1.join();
}

/**
 * @brief
 * 1.源点云进树,遍历目标点云画圈搜索
 * 2.所有的索引合并去重
 * 3.根据索引将源点云过滤
 * 4.目标点云进树,过滤后源点云遍历搜索
 * 5.所有的索引合并去重
 * 6.根据索引将目标点云过滤
 *
 * @param source_cloud
 * @param target_cloud
 */
void IcpMethodCali::FindCoView(
    pcl::PointCloud<pcl::PointXYZI>::Ptr source_cloud,
    pcl::PointCloud<pcl::PointXYZI>::Ptr target_cloud) {
  pcl::PointCloud<pcl::PointXYZI>::Ptr source_cloud_filtered(
      new pcl::PointCloud<pcl::PointXYZI>);
  pcl::PointCloud<pcl::PointXYZI>::Ptr target_cloud_filtered(
      new pcl::PointCloud<pcl::PointXYZI>);

  pcl::KdTreeFLANN<pcl::PointXYZI>::Ptr s_kdtree(
      new pcl::KdTreeFLANN<pcl::PointXYZI>);
  pcl::KdTreeFLANN<pcl::PointXYZI>::Ptr t_kdtree(
      new pcl::KdTreeFLANN<pcl::PointXYZI>);

  std::vector<int> s_idx_total_vec;
  std::vector<int> t_idx_total_vec;

  std::vector<int> s_idx_vec;
  std::vector<int> t_idx_vec;

  std::vector<float> s_distance_vec;
  std::vector<float> t_distance_vec;

  std::thread s_t;
  std::thread t_t;

  double radius = 20;

  s_t = std::thread([&]() {
    s_kdtree->setInputCloud(source_cloud);
    for (size_t i = 0; i < target_cloud->size(); i++) {
      s_kdtree->nearestKSearch(target_cloud->points[i], radius, s_idx_vec,
                               s_distance_vec);
      std::copy(s_idx_vec.begin(), s_idx_vec.end(),
                std::back_inserter(s_idx_total_vec));
    }
    std::sort(s_idx_total_vec.begin(), s_idx_total_vec.end());
    auto it = std::unique(s_idx_total_vec.begin(), s_idx_total_vec.end());
    s_idx_total_vec.erase(it, s_idx_total_vec.end());
    for (size_t i = 0; i < s_idx_total_vec.size(); i++) {
      source_cloud_filtered->push_back(
          source_cloud->points[s_idx_total_vec[i]]);
    }
    source_cloud = source_cloud_filtered;
  });

  t_t = std::thread([&]() {
    t_kdtree->setInputCloud(target_cloud);
    for (size_t i = 0; i < source_cloud->size(); i++) {
      t_kdtree->nearestKSearch(source_cloud->points[i], radius, t_idx_vec,
                               t_distance_vec);
      std::copy(t_idx_vec.begin(), t_idx_vec.end(),
                std::back_inserter(t_idx_total_vec));
    }
    std::sort(t_idx_total_vec.begin(), t_idx_total_vec.end());
    auto it = std::unique(t_idx_total_vec.begin(), t_idx_total_vec.end());
    t_idx_total_vec.erase(it, t_idx_total_vec.end());

    for (size_t i = 0; i < t_idx_total_vec.size(); i++) {
      target_cloud_filtered->push_back(
          target_cloud->points[t_idx_total_vec[i]]);
    }
    target_cloud = target_cloud_filtered;
  });

  s_t.join();
  t_t.join();
}

uint8_t IcpMethodCali::CheckHeader(std_msgs::msg::Header header_0,
                                   std_msgs::msg::Header header_1) {
  double time_ms_0 = header_0.stamp.nanosec / 1000000.0;
  double time_ms_1 = header_1.stamp.nanosec / 1000000.0;
  if ((time_ms_0 - time_ms_1) > 0.0 && (time_ms_0 - time_ms_1) > 100.0) {
    // 1 too old
    return 1;
  } else if ((time_ms_0 - time_ms_1) < 0.0 &&
             (time_ms_0 - time_ms_1) < -100.0) {
    // 0 too old
    return 0;
  } else {
    return 100;
  }
}

}  // namespace calibrate