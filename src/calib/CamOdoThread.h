#ifndef CAMODOTHREAD_H
#define CAMODOTHREAD_H

#include <glibmm.h>

#include "camodocal/calib/AtomicData.h"
#include "camodocal/calib/CamOdoCalibration.h"
#include "camodocal/calib/PoseSource.h"
#include "camodocal/calib/SensorDataBuffer.h"
#include "camodocal/camera_models/Camera.h"
#include "camodocal/sparse_graph/SparseGraph.h"

namespace camodocal
{

class CamOdoThread
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    explicit CamOdoThread(PoseSource poseSource, int nMotions, int cameraId,
                          bool preprocess,
                          AtomicData<cv::Mat>* image,
                          const CameraConstPtr& camera,
                          SensorDataBuffer<OdometryPtr>& odometryBuffer,
                          SensorDataBuffer<OdometryPtr>& interpOdometryBuffer,
                          boost::mutex& odometryBufferMutex,
                          SensorDataBuffer<PosePtr>& gpsInsBuffer,
                          SensorDataBuffer<PosePtr>& interpGpsInsBuffer,
                          boost::mutex& gpsInsBufferMutex,
                          std::string& status,
                          cv::Mat& sketch,
                          bool& completed,
                          bool& stop,
                          bool verbose = false);
    virtual ~CamOdoThread();

    int cameraId(void) const;
    const Eigen::Matrix4d& camOdoTransform(void) const;
    const std::vector<std::vector<FramePtr> >& frameSegments(void) const;

    void reprojectionError(double& minError, double& maxError, double& avgError) const;

    void launch(void);
    void join(void);
    bool running(void) const;
    sigc::signal<void>& signalFinished(void);

private:
    void threadFunction(void);

    void addCamOdoCalibData(const std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> >& camPoses,
                            const std::vector<OdometryPtr>& odoPoses,
                            std::vector<FramePtr>& frameSegment);

    PoseSource m_poseSource;

    Glib::Threads::Thread* m_thread;
    int m_cameraId;
    bool m_preprocess;
    bool m_running;
    sigc::signal<void> m_signalFinished;

    CamOdoCalibration m_camOdoCalib;
    std::vector<std::vector<FramePtr> > m_frameSegments;

    AtomicData<cv::Mat>* m_image;
    const CameraConstPtr m_camera;
    SensorDataBuffer<OdometryPtr>& m_odometryBuffer;
    SensorDataBuffer<OdometryPtr>& m_interpOdometryBuffer;
    boost::mutex& m_odometryBufferMutex;
    SensorDataBuffer<PosePtr>& m_gpsInsBuffer;
    SensorDataBuffer<PosePtr>& m_interpGpsInsBuffer;
    boost::mutex& m_gpsInsBufferMutex;
    Eigen::Matrix4d m_camOdoTransform;
    std::string& m_status;
    cv::Mat& m_sketch;

    const double k_keyFrameDistance;
    const int k_minTrackLength;
    const double k_odometryTimeout;

    bool& m_completed;
    bool& m_stop;
};

}

#endif
