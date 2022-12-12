// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is an implementation of the algorithm described in the following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

#include "loam_velodyne/MultiScanRegistration.h"
#include "math_utils.h"
#include <fstream>
#include <pcl_conversions/pcl_conversions.h>


namespace loam {

MultiScanMapper::MultiScanMapper(const float& lowerBound,
                                 const float& upperBound,
                                 const uint16_t& nScanRings)
    : _lowerBound(lowerBound),
      _upperBound(upperBound),
      _nScanRings(nScanRings),
      _factor((nScanRings - 1) / (upperBound - lowerBound))
{

}



void MultiScanMapper::set(const float &lowerBound,
                          const float &upperBound,
                          const uint16_t &nScanRings)
{
  _lowerBound = lowerBound;
  _upperBound = upperBound;
  _nScanRings = nScanRings;
  _factor = (nScanRings - 1) / (upperBound - lowerBound);
}



int MultiScanMapper::getRingForAngle(const float& angle) {
  return int(((angle * 180 / M_PI) - _lowerBound) * _factor + 0.5);
}

MultiScanRegistration::MultiScanRegistration(const MultiScanMapper& scanMapper,
                                             const RegistrationParams& config)
    : ScanRegistration(config),
      _systemDelay(SYSTEM_DELAY),
      _scanMapper(scanMapper)
{

};



bool MultiScanRegistration::setup(ros::NodeHandle& node,
                                  ros::NodeHandle& privateNode)
{
  if (!ScanRegistration::setup(node, privateNode)) {
    return false;
  }

  // fetch scan mapping params
  std::string lidarName;

  if (privateNode.getParam("lidar", lidarName)) {
    if (lidarName == "VLP-16") {
      _scanMapper = MultiScanMapper::Velodyne_VLP_16();
    } else if (lidarName == "HDL-32") {
      _scanMapper = MultiScanMapper::Velodyne_HDL_32();
    } else if (lidarName == "HDL-64E") {
      _scanMapper = MultiScanMapper::Velodyne_HDL_64E();
    } else {
      ROS_ERROR("Invalid lidar parameter: %s (only \"VLP-16\", \"HDL-32\" and \"HDL-64E\" are supported)", lidarName.c_str());
      return false;
    }

    ROS_INFO("Set  %s  scan mapper.", lidarName.c_str());
    if (!privateNode.hasParam("scanPeriod")) {
      _config.scanPeriod = 0.1;
      ROS_INFO("Set scanPeriod: %f", _config.scanPeriod);
    }
  } else {
    float vAngleMin, vAngleMax;
    int nScanRings;

    if (privateNode.getParam("minVerticalAngle", vAngleMin) &&
        privateNode.getParam("maxVerticalAngle", vAngleMax) &&
        privateNode.getParam("nScanRings", nScanRings)) {
      if (vAngleMin >= vAngleMax) {
        ROS_ERROR("Invalid vertical range (min >= max)");
        return false;
      } else if (nScanRings < 2) {
        ROS_ERROR("Invalid number of scan rings (n < 2)");
        return false;
      }

      _scanMapper.set(vAngleMin, vAngleMax, nScanRings);
      ROS_INFO("Set linear scan mapper from %g to %g degrees with %d scan rings.", vAngleMin, vAngleMax, nScanRings);
    }
  }

  // subscribe to input cloud topic
  _subLaserCloud = node.subscribe<sensor_msgs::PointCloud2>
      ("/multi_scan_points", 2, &MultiScanRegistration::handleCloudMessage, this);

  return true;
}



void MultiScanRegistration::handleCloudMessage(const sensor_msgs::PointCloud2ConstPtr &laserCloudMsg)
{
  if (_systemDelay > 0) {
    _systemDelay--;
    return;
  }

  // fetch new input cloud
  pcl::PointCloud<pcl::PointXYZ> laserCloudIn;
  pcl::fromROSMsg(*laserCloudMsg, laserCloudIn);

  process(laserCloudIn, laserCloudMsg->header.stamp);
}


/*
void convertKITTI(pcl::PointXYZ& point)
{
  float z = point.x;
  float x = point.y;
  float y = point.z;
  point.x = x;
  point.y = y;
  point.z = z;
}
*/


void MultiScanRegistration::process(pcl::PointCloud<pcl::PointXYZ>& laserCloudIn,
                                    const ros::Time& scanTime)
{
  size_t cloudSize = laserCloudIn.size();

  // reset internal buffers and set IMU start state based on current scan time
  reset(scanTime);

  // convertKITTI(laserCloudIn[0]);
  // convertKITTI(laserCloudIn[cloudSize - 1]);
  pcl::PointXYZI pointStart;
  pointStart.z = laserCloudIn[0].x;
  pointStart.x = laserCloudIn[0].y;
  pointStart.y = laserCloudIn[0].z;
  pcl::PointXYZI pointEnd;
  pointEnd.z = laserCloudIn[cloudSize - 1].x;
  pointEnd.x = laserCloudIn[cloudSize - 1].y;
  pointEnd.y = laserCloudIn[cloudSize - 1].z;

    /**
     * 三维扫描仪并不像二维那样按照角度给出个距离值，从而保证每次的扫描都有
     * 相同的数据量。 PointCloud2接受到的点云的大小在变化，因此在数据到达时
     * 需要一些运算来判断点的一些特征。
     */

  // determine scan start and end orientations
  //float startOri = -std::atan2(laserCloudIn[0].y, laserCloudIn[0].x);
  //float endOri = -std::atan2(laserCloudIn[cloudSize - 1].y, laserCloudIn[cloudSize - 1].x) + 2 * float(M_PI);
  /*float startOri = -std::atan2(pointStart.x, pointStart.z);
  float endOri = -std::atan2(pointEnd.x, pointEnd.z) + 2 * float(M_PI);

  if (endOri - startOri > 3 * M_PI) {
    endOri -= 2 * M_PI;
  } else if (endOri - startOri < M_PI) {
    endOri += 2 * M_PI;
  }*/

  // TODO: evaluate effect of change
  float startOri = 0.0;
  float endOri = 2 * float(M_PI);

  bool halfPassed = false;
  int counter_invalid_point = 0;
  int scanID = 63;
  bool negPassed = false;
  bool posPassed = false;
  float peakThresh = M_PI / 4.0;
  pcl::PointXYZI point, pointPrev;

  /* 将点划到不同的线中 */
  std::vector<pcl::PointCloud<pcl::PointXYZI> > laserCloudScans(_scanMapper.getNumberOfScanRings());

  // extract valid points from input cloud
  for (int i = 0; i < cloudSize; i++) {

    // convertKITTI(laserCloudIn[i]);
    point.x = laserCloudIn[i].y;
    point.y = laserCloudIn[i].z;
    point.z = laserCloudIn[i].x;

    // skip NaN and INF valued points
    if (!pcl_isfinite(point.x) ||
        !pcl_isfinite(point.y) ||
        !pcl_isfinite(point.z)) {
      continue;
    }

    // skip zero valued points
    if (point.x * point.x + point.y * point.y + point.z * point.z < 0.0001) {
      continue;
    }

    // calculate vertical point angle and scan ID
    /*float angle = std::atan(point.y / std::sqrt(point.x * point.x + point.z * point.z));
    int scanID = _scanMapper.getRingForAngle(angle);
    if (scanID >= _scanMapper.getNumberOfScanRings() || scanID < 0 ){
      continue;
    }*/

    // calculate horizontal point angle
    float ori = -std::atan2(point.x, point.z);

    if (i > 0) {
      if (ori < - peakThresh) {
        negPassed = true;
      }
      if (ori > peakThresh) {
        posPassed = true;
      }
      float oriPrev = -std::atan2(pointPrev.x, pointPrev.z);
      if (ori < 0.0 && oriPrev > 0.0 && negPassed && posPassed) {
        scanID--;
        negPassed = false;
        posPassed = false;
      }
    }

    if (scanID < 0 ) {
      /*std::ofstream myfile;
      myfile.open ("/home/cedricxie/Documents/Udacity/Didi_Challenge/catkin_ws/ros_bags/kitti/odometry/point_cloud.txt", std::ios_base::app);
      for (int ii = 0; ii < cloudSize; ii++) {
        pcl::PointXYZI pointTemp;
        pointTemp.x = laserCloudIn[ii].y;
        pointTemp.y = laserCloudIn[ii].z;
        pointTemp.z = laserCloudIn[ii].x;
        float oriTemp = -std::atan2(pointTemp.x, pointTemp.z);
        myfile << ii << " " << oriTemp << " " << pointTemp.x << " " << pointTemp.z << " " << scanID << " \n";
      }
      myfile.close();*/
      ROS_INFO("[multiScanRegistration] Too many scanID, %d", scanID );
      break;
    }

    /*if (!halfPassed) {
      if (ori < startOri - M_PI / 2) {
        ori += 2 * M_PI;
      } else if (ori > startOri + M_PI * 3 / 2) {
        ori -= 2 * M_PI;
      }

      if (ori - startOri > M_PI) {
        halfPassed = true;
      }
    } else {
      ori += 2 * M_PI;

      if (ori < endOri - M_PI * 3 / 2) {
        ori += 2 * M_PI;
      } else if (ori > endOri + M_PI / 2) {
        ori -= 2 * M_PI;
      }
    }*/
    if (ori > 0.0) {
      ori = 2.0 * M_PI - ori;
    } else {
      ori = - ori;
    }

    /*
    IMPORTANT NOTE: Note that the velodyne scanner takes depth measurements
    continuously while rotating around its vertical axis (in contrast to the cameras,
    which are triggered at a certain point in time). This effect has been
    eliminated from this postprocessed data by compensating for the egomotion!!
    Note that this is in contrast to the raw data.
    */

    // calculate relative scan time based on point orientation
    // float relTime = _config.scanPeriod * (ori - startOri) / (endOri - startOri);
    // float relTime =  0.0;
    float relTime =  _config.scanPeriod * 0.99;
    // ROS_INFO("[multiScanRegistration] ori %d, %d, %f, %f", scanID, i, ori, relTime);

    if (relTime < 0 )
    {
      // relTime = _config.scanPeriod * (ori + 2.0 * M_PI - startOri) / (endOri - startOri);
      // ROS_INFO("[multiScanRegistration] relTime %f, %f, %f, %f, %f, %d", -std::atan2(point.x, point.z), ori, startOri, -std::atan2(pointEnd.x, pointEnd.z), endOri, halfPassed);
      counter_invalid_point++;
    }
    if (relTime > 0.1 )
    {
      // relTime = _config.scanPeriod * (ori - 2.0 * M_PI - startOri) / (endOri - startOri);
      //ROS_INFO("[multiScanRegistration] relTime %f, %f, %f, %f, %f, %d", -std::atan2(point.x, point.z), ori, startOri, -std::atan2(pointEnd.x, pointEnd.z), endOri, halfPassed);
      counter_invalid_point++;
      // continue;
    }

    point.intensity = scanID + relTime;

    // project point to the start of the sweep using corresponding IMU data
    if (hasIMUData()) {
      setIMUTransformFor(relTime);
      transformToStartIMU(point);
    }

    pointPrev = point;

    laserCloudScans[scanID].push_back(point);
  }

  if (counter_invalid_point > 0) {
    ROS_INFO("[multiScanRegistration] invalid point %d, out of %d", counter_invalid_point, int(cloudSize));
  }

  // construct sorted full resolution cloud
  cloudSize = 0;
  for (int i = 0; i < _scanMapper.getNumberOfScanRings(); i++) {
    _laserCloud += laserCloudScans[i];

    IndexRange range(cloudSize, 0);
    cloudSize += laserCloudScans[i].size();
    range.second = cloudSize > 0 ? cloudSize - 1 : 0;
    _scanIndices.push_back(range);
  }

  // extract features
  extractFeatures();

  // publish result
  publishResult();
}

} // end namespace loam
