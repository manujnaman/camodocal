#include "CamOdoThread.h"

#include <iostream>

#include "../gpl/EigenUtils.h"
#include "../visual_odometry/FeatureTracker.h"
#include "utils.h"
#ifdef VCHARGE_VIZ
#include "../../../../library/gpl/CameraEnums.h"
#include "../../../../visualization/overlay/GLOverlayExtended.h"
#include "CalibrationWindow.h"
#endif

namespace camodocal
{

CamOdoThread::CamOdoThread(PoseSource poseSource, int nMotions, int cameraId,
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
                           bool verbose)
 : m_poseSource(poseSource)
 , m_thread(0)
 , m_cameraId(cameraId)
 , m_running(false)
 , m_preprocess(preprocess)
 , m_image(image)
 , m_camera(camera)
 , m_odometryBuffer(odometryBuffer)
 , m_interpOdometryBuffer(interpOdometryBuffer)
 , m_odometryBufferMutex(odometryBufferMutex)
 , m_gpsInsBuffer(gpsInsBuffer)
 , m_interpGpsInsBuffer(interpGpsInsBuffer)
 , m_gpsInsBufferMutex(gpsInsBufferMutex)
 , m_status(status)
 , m_sketch(sketch)
 , k_keyFrameDistance(0.25)
 , k_minTrackLength(15)
 , k_odometryTimeout(4.0)
 , m_completed(completed)
 , m_stop(stop)
{
    m_camOdoCalib.setVerbose(verbose);
    m_camOdoCalib.setMotionCount(nMotions);
}

CamOdoThread::~CamOdoThread()
{
    g_return_if_fail(m_thread == 0);
}

int
CamOdoThread::cameraId(void) const
{
    return m_cameraId;
}

const Eigen::Matrix4d&
CamOdoThread::camOdoTransform(void) const
{
    return m_camOdoTransform;
}

const std::vector<std::vector<FramePtr> >&
CamOdoThread::frameSegments(void) const
{
    return m_frameSegments;
}

void
CamOdoThread::reprojectionError(double& minError, double& maxError, double& avgError) const
{
    minError = std::numeric_limits<double>::max();
    maxError = std::numeric_limits<double>::min();

    size_t count = 0;
    double totalError = 0.0;

    for (size_t segmentId = 0; segmentId < m_frameSegments.size(); ++segmentId)
    {
        const std::vector<FramePtr>& segment = m_frameSegments.at(segmentId);

        for (size_t frameId = 0; frameId < segment.size(); ++frameId)
        {
            const FrameConstPtr& frame = segment.at(frameId);

            const std::vector<Point2DFeaturePtr>& features2D = frame->features2D();

            for (size_t i = 0; i < features2D.size(); ++i)
            {
                const Point2DFeatureConstPtr& feature2D = features2D.at(i);
                const Point3DFeatureConstPtr& feature3D = feature2D->feature3D();

                if (feature3D.get() == 0)
                {
                    continue;
                }

                double error = m_camera->reprojectionError(feature3D->point(),
                                                           frame->cameraPose()->rotation(),
                                                           frame->cameraPose()->translation(),
                                                           Eigen::Vector2d(feature2D->keypoint().pt.x, feature2D->keypoint().pt.y));

                if (minError > error)
                {
                    minError = error;
                }
                if (maxError < error)
                {
                    maxError = error;
                }
                totalError += error;
                ++count;
            }
        }
    }

    if (count == 0)
    {
        avgError = 0.0;
        minError = 0.0;
        maxError = 0.0;

        return;
    }

    avgError = totalError / count;
}

void
CamOdoThread::launch(void)
{
    m_thread = Glib::Threads::Thread::create(sigc::mem_fun(*this, &CamOdoThread::threadFunction));
}

void
CamOdoThread::join(void)
{
    if (m_running)
    {
        m_thread->join();
    }
    m_thread = 0;
}

bool
CamOdoThread::running(void) const
{
    return m_running;
}

sigc::signal<void>&
CamOdoThread::signalFinished(void)
{
    return m_signalFinished;
}

void
CamOdoThread::threadFunction(void)
{
    m_running = true;

    TemporalFeatureTracker tracker(m_camera,
                                   SURF_GPU_DETECTOR, SURF_GPU_DESCRIPTOR,
                                   RATIO_GPU, m_preprocess);
    tracker.setVerbose(m_camOdoCalib.getVerbose());

    FramePtr framePrev;

    cv::Mat image;
    cv::Mat colorImage;

    int trackBreaks = 0;

    std::vector<OdometryPtr> odometryPoses;

#ifdef VCHARGE_VIZ
    std::ostringstream oss;
    oss << "swba" << m_cameraId + 1;
    vcharge::GLOverlayExtended overlay(oss.str(), VCharge::COORDINATE_FRAME_GLOBAL);
#endif

    bool halt = false;

    while (!halt)
    {
        boost::system_time timeout = boost::get_system_time() + boost::posix_time::milliseconds(10);
        while (!m_image->timedWaitForData(timeout) && !m_stop)
        {
            timeout = boost::get_system_time() + boost::posix_time::milliseconds(10);
        }

        if (m_stop)
        {
            std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > voPoses = tracker.getPoses();

            if (odometryPoses.size() >= k_minTrackLength)
            {
                addCamOdoCalibData(voPoses, odometryPoses, tracker.getFrames());
            }

            if (!odometryPoses.empty())
            {
                odometryPoses.erase(odometryPoses.begin(), odometryPoses.begin() + voPoses.size() - 1);
            }

            ++trackBreaks;

            halt = true;
        }
        else
        {
            m_image->lockData();
            m_image->available() = false;

            uint64_t timeStamp = m_image->timeStamp();

            if (framePrev.get() != 0 && timeStamp == framePrev->cameraPose()->timeStamp())
            {
                m_image->unlockData();
                m_image->notifyProcessingDone();

                continue;
            }

            m_image->data().copyTo(image);

            m_image->unlockData();

            if (image.channels() == 1)
            {
                cv::cvtColor(image, colorImage, CV_GRAY2BGR);
            }
            else
            {
                image.copyTo(colorImage);
            }

            // skip if current car position is too near previous position
            OdometryPtr currOdometry;
            PosePtr currGpsIns;
            Eigen::Vector2d pos;

            if (m_poseSource == ODOMETRY && !m_odometryBuffer.current(currOdometry))
            {
                std::cout << "# WARNING: No data in odometry buffer." << std::endl;
            }
            else if (m_poseSource == GPS_INS && !m_gpsInsBuffer.current(currGpsIns))
            {
                std::cout << "# WARNING: No data in GPS/INS buffer." << std::endl;
            }
            else
            {
                m_odometryBufferMutex.lock();

                OdometryPtr interpOdo;
                if (m_poseSource == ODOMETRY && !m_interpOdometryBuffer.find(timeStamp, interpOdo))
                {
                    double timeStart = timeInSeconds();
                    while (!interpolateOdometry(m_odometryBuffer, timeStamp, interpOdo))
                    {
                        if (timeInSeconds() - timeStart > k_odometryTimeout)
                        {
                            std::cout << "# ERROR: No odometry data for " << k_odometryTimeout << "s. Exiting..." << std::endl;
                            exit(1);
                        }

                        usleep(1000);
                    }

                    m_interpOdometryBuffer.push(timeStamp, interpOdo);
                }

                m_odometryBufferMutex.unlock();

                m_gpsInsBufferMutex.lock();

                PosePtr interpGpsIns;
                if ((m_poseSource == GPS_INS || !m_gpsInsBuffer.empty()) && !m_interpGpsInsBuffer.find(timeStamp, interpGpsIns))
                {
                    double timeStart = timeInSeconds();
                    while (!interpolatePose(m_gpsInsBuffer, timeStamp, interpGpsIns))
                    {
                        if (timeInSeconds() - timeStart > k_odometryTimeout)
                        {
                            std::cout << "# ERROR: No GPS/INS data for " << k_odometryTimeout << "s. Exiting..." << std::endl;
                            exit(1);
                        }

                        usleep(1000);
                    }

                    m_interpGpsInsBuffer.push(timeStamp, interpGpsIns);
                }

                m_gpsInsBufferMutex.unlock();

                Eigen::Vector3d pos;
                if (m_poseSource == ODOMETRY)
                {
                    pos = interpOdo->position();
                }
                else
                {
                    pos(0) = interpGpsIns->translation()(1);
                    pos(1) = -interpGpsIns->translation()(0);
                    pos(2) = interpGpsIns->translation()(2);
                }

                if (framePrev.get() != 0 &&
                    (pos - framePrev->systemPose()->position()).norm() < k_keyFrameDistance)
                {
                    m_image->notifyProcessingDone();
                    continue;
                }

                FramePtr frame(new Frame);
                frame->cameraId() = m_cameraId;
                image.copyTo(frame->image());

                Eigen::Matrix3d R;
                Eigen::Vector3d t;
                bool camValid = tracker.addFrame(frame, m_camera->mask(), R, t);

                // tag frame with odometry and GPS/INS data
                frame->odometryMeasurement().reset(new Odometry);
                *(frame->odometryMeasurement()) = *interpOdo;
                frame->systemPose().reset(new Odometry);
                *(frame->systemPose()) = *interpOdo;

                if (interpGpsIns.get() != 0)
                {
                    frame->gpsInsMeasurement() = interpGpsIns;
                }

                if (m_poseSource == GPS_INS)
                {
                    OdometryPtr gpsIns(new Odometry);
                    gpsIns->timeStamp() = interpGpsIns->timeStamp();
                    gpsIns->x() = interpGpsIns->translation()(1);
                    gpsIns->y() = -interpGpsIns->translation()(0);

                    Eigen::Matrix3d R = interpGpsIns->rotation().toRotationMatrix();
                    double roll, pitch, yaw;
                    mat2RPY(R, roll, pitch, yaw);
                    gpsIns->yaw() = -yaw;

                    frame->odometryMeasurement().reset(new Odometry);
                    *(frame->odometryMeasurement()) = *gpsIns;
                    frame->systemPose().reset(new Odometry);
                    *(frame->systemPose()) = *gpsIns;
                }

                frame->cameraPose()->timeStamp() = timeStamp;

                if (camValid)
                {
                    odometryPoses.push_back(frame->systemPose());
                }

                framePrev = frame;

                if (!camValid)
                {
                    std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > voPoses = tracker.getPoses();

                    if (odometryPoses.size() >= k_minTrackLength)
                    {
                        addCamOdoCalibData(voPoses, odometryPoses, tracker.getFrames());
                    }

                    if (!odometryPoses.empty())
                    {
                        odometryPoses.erase(odometryPoses.begin(), odometryPoses.begin() + voPoses.size() - 1);
                    }

                    ++trackBreaks;
                }
            }
        }

#ifdef VCHARGE_VIZ
        {
            // visualize camera poses and 3D scene points
            const std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> >& poses = tracker.getPoses();

            overlay.clear();
            overlay.pointSize(2.0f);
            overlay.lineWidth(1.0f);

            // draw 3D scene points
//                switch (cameraId)
//                {
//                case CAMERA_FRONT:
//                    overlay.color4f(1.0f, 0.0f, 0.0f, 0.5f);
//                    break;
//                case CAMERA_LEFT:
//                    overlay.color4f(0.0f, 1.0f, 0.0f, 0.5f);
//                    break;
//                case CAMERA_REAR:
//                    overlay.color4f(0.0f, 0.0f, 1.0f, 0.5f);
//                    break;
//                case CAMERA_RIGHT:
//                    overlay.color4f(1.0f, 1.0f, 0.0f, 0.5f);
//                    break;
//                default:
//                    overlay.color4f(1.0f, 1.0f, 1.0f, 0.5f);
//                }
//
//                overlay.begin(VCharge::POINTS);
//
//                std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > scenePoints = tracker.getScenePoints();
//                for (size_t j = 0; j < scenePoints.size(); ++j)
//                {
//                    Eigen::Vector3d& p = scenePoints.at(j);
//
//                    overlay.vertex3f(p(2), -p(0), -p(1));
//                }
//
//                overlay.end();

            // draw cameras
            for (size_t j = 0; j < poses.size(); ++j)
            {
                Eigen::Matrix4d H = poses.at(j).inverse();

                double xBound = 0.1;
                double yBound = 0.1;
                double zFar = 0.2;

                std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > frustum;
                frustum.push_back(Eigen::Vector3d(0.0, 0.0, 0.0));
                frustum.push_back(Eigen::Vector3d(-xBound, -yBound, zFar));
                frustum.push_back(Eigen::Vector3d(xBound, -yBound, zFar));
                frustum.push_back(Eigen::Vector3d(xBound, yBound, zFar));
                frustum.push_back(Eigen::Vector3d(-xBound, yBound, zFar));

                for (size_t k = 0; k < frustum.size(); ++k)
                {
                    frustum.at(k) = H.block<3,3>(0,0) * frustum.at(k) + H.block<3,1>(0,3);
                }

                overlay.color4f(1.0f, 1.0f, 1.0f, 1.0f);
                overlay.begin(VCharge::LINES);

                for (int k = 1; k < 5; ++k)
                {
                    overlay.vertex3f(frustum.at(0)(2), -frustum.at(0)(0), -frustum.at(0)(1));
                    overlay.vertex3f(frustum.at(k)(2), -frustum.at(k)(0), -frustum.at(k)(1));
                }

                overlay.end();

                switch (m_cameraId)
                {
                case vcharge::CAMERA_FRONT:
                    overlay.color4f(1.0f, 0.0f, 0.0f, 0.5f);
                    break;
                case vcharge::CAMERA_LEFT:
                    overlay.color4f(0.0f, 1.0f, 0.0f, 0.5f);
                    break;
                case vcharge::CAMERA_REAR:
                    overlay.color4f(0.0f, 0.0f, 1.0f, 0.5f);
                    break;
                case vcharge::CAMERA_RIGHT:
                    overlay.color4f(1.0f, 1.0f, 0.0f, 0.5f);
                    break;
                default:
                    overlay.color4f(1.0f, 1.0f, 1.0f, 0.5f);
                }

                overlay.begin(VCharge::POLYGON);

                for (int k = 1; k < 5; ++k)
                {
                    overlay.vertex3f(frustum.at(k)(2), -frustum.at(k)(0), -frustum.at(k)(1));
                }

                overlay.end();
            }

            overlay.publish();
        }
#endif

        int currentMotionCount = 0;
        if (odometryPoses.size() >= k_minTrackLength)
        {
            currentMotionCount = odometryPoses.size() - 1;
        }

        std::ostringstream oss;
        oss << "# motions: " << m_camOdoCalib.getCurrentMotionCount() + currentMotionCount << " | "
            << "# track breaks: " << trackBreaks;

#ifdef VCHARGE_VIZ
        CalibrationWindow::instance()->dataMutex().lock();

        m_status.assign(oss.str());

        if (!tracker.getSketch().empty())
        {
            tracker.getSketch().copyTo(m_sketch);
        }
        else
        {
            colorImage.copyTo(m_sketch);
        }

        CalibrationWindow::instance()->dataMutex().unlock();
#endif

        m_image->notifyProcessingDone();

        if (m_camOdoCalib.getCurrentMotionCount() + currentMotionCount >= m_camOdoCalib.getMotionCount())
        {
            m_completed = true;
        }
    }

    std::cout << "# INFO: Calibrating odometry - camera " << m_cameraId << "..." << std::endl;

//    m_camOdoCalib.writeMotionSegmentsToFile(filename);

    Eigen::Matrix4d H_cam_odo;
    m_camOdoCalib.calibrate(H_cam_odo);

    std::cout << "# INFO: Finished calibrating odometry - camera " << m_cameraId << "..." << std::endl;
    std::cout << "Rotation: " << std::endl << H_cam_odo.block<3,3>(0,0) << std::endl;
    std::cout << "Translation: " << std::endl << H_cam_odo.block<3,1>(0,3).transpose() << std::endl;

    m_camOdoTransform = H_cam_odo;

    m_running = false;

    m_signalFinished();
}

void
CamOdoThread::addCamOdoCalibData(const std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> >& camPoses,
                                 const std::vector<OdometryPtr>& odoPoses,
                                 std::vector<FramePtr>& frameSegment)
{
    if (odoPoses.size() != camPoses.size())
    {
        std::cout << "# WARNING: Numbers of odometry (" << odoPoses.size()
                  << ") and camera poses (" << camPoses.size() << ") differ. Aborting..." << std::endl;

        return;
    }

    if (odoPoses.size() < k_minTrackLength)
    {
        std::cout << "# WARNING: At least " << k_minTrackLength << " poses are needed. Aborting..." << std::endl;

        return;
    }

    std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > odoMotions;
    std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > camMotions;

    for (size_t i = 1; i < odoPoses.size(); ++i)
    {
        Eigen::Matrix4d relativeOdometryPose = odoPoses.at(i)->toMatrix().inverse() * odoPoses.at(i - 1)->toMatrix();
        odoMotions.push_back(relativeOdometryPose);

        Eigen::Matrix4d relativeCameraPose = camPoses.at(i) * camPoses.at(i - 1).inverse();
        camMotions.push_back(relativeCameraPose);
    }

    if (!m_camOdoCalib.addMotionSegment(camMotions, odoMotions))
    {
        std::cout << "# ERROR: Numbers of odometry and camera motions do not match." << std::endl;
        exit(0);
    }

    m_frameSegments.push_back(frameSegment);
}

}
