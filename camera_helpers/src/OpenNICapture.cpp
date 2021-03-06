#include <camera_helpers/OpenNICapture.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud2.h>
#include <boost/thread/locks.hpp>
#include <pcl/ros/conversions.h>
#include <boost/thread.hpp>

#include <ros/callback_queue.h>



namespace enc = sensor_msgs::image_encodings;

using namespace std ;
using namespace sensor_msgs;
using namespace message_filters;

namespace camera_helpers {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class OpenNICaptureImpl {
public:
    OpenNICaptureImpl(const std::string &prefix_): prefix(prefix_),
        connected(false), dataReady(false),nh("~") {

        nh.setCallbackQueue( &callback_queue_ );


    }


    bool connect(ros::Duration timeout) ;
    void connect(boost::function<void ()> cb, ros::Duration timeout) ;
    virtual void disconnect() ;
    bool isConnected() const {
        boost::unique_lock<boost::mutex> lock_ (image_lock) ;
        return connected ;
    }

protected:
    virtual void setup() = 0;
    virtual void shutdown() = 0;

    void waitForConnection(ros::Duration t) ;
    void connectCb(boost::function<void ()> cb, ros::Duration t) ;
    void spinThread() ;

    std::string prefix ;

    ros::NodeHandle nh;

    mutable boost::mutex image_lock ;
    mutable boost::timed_mutex data_ready ;

    bool connected, dataReady ;
    ros::CallbackQueue callback_queue_;

    boost::scoped_ptr<boost::thread> spin_thread_;
    bool need_to_terminate_ ;

};

void OpenNICaptureImpl::spinThread()
{
    while (nh.ok())
    {
        if (need_to_terminate_)
        {
            break;
        }

        callback_queue_.callAvailable(ros::WallDuration(0.033f));
    }
}



bool OpenNICaptureImpl::connect(ros::Duration timeout)
{
    if ( connected ) disconnect() ;

    setup() ;

    data_ready.lock() ;

    spin_thread_.reset( new boost::thread(boost::bind(&OpenNICaptureImpl::spinThread, this)) );


    if ( timeout.toSec() == -1 )
    {
        boost::unique_lock<boost::timed_mutex> lock_ (data_ready) ;
        connected = true ;
    }
    else
    {

        boost::unique_lock<boost::timed_mutex> lock_ (data_ready, timeout.toBoost()) ;
        connected = lock_.owns_lock() ;
    }


    connected = true ;

    return connected ;
}

void OpenNICaptureImpl::connectCb(boost::function<void ()> cb, ros::Duration timeout)
{
    if ( timeout.toSec() == -1 )
    {
        boost::unique_lock<boost::timed_mutex> lock_ (data_ready) ;
        connected = true ;
    }
    else
    {

        boost::unique_lock<boost::timed_mutex> lock_ (data_ready, timeout.toBoost()) ;
        connected = lock_.owns_lock() ;
    }

    if ( !connected ) {
        disconnect() ;
        return ;
    }


    cb() ;

}

void OpenNICaptureImpl::connect(boost::function<void ()> cb, ros::Duration timeout)
{
    {
        boost::unique_lock<boost::mutex> lock_ (image_lock) ;

        if ( connected ) disconnect() ;
    }

    setup() ;

    data_ready.lock() ;

    spin_thread_.reset( new boost::thread(boost::bind(&OpenNICaptureImpl::spinThread, this)) );

    // start connection thread

    boost::thread t(boost::bind(&OpenNICaptureImpl::connectCb, this, cb, timeout)) ;
}



void OpenNICaptureImpl::disconnect()
{

    boost::unique_lock<boost::mutex> lock_ (image_lock) ;

    if ( connected )
    {
        shutdown() ;

        if (spin_thread_.get())
        {
            need_to_terminate_ = true;
            spin_thread_->join();
        }
    }

    connected = false ;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class OpenNICaptureImplRGBD: public OpenNICaptureImpl
{
public:

    OpenNICaptureImplRGBD(const string &prefix_): OpenNICaptureImpl(prefix_),
        sync(sync_policies::ApproximateTime<Image, Image, CameraInfo>(10), rgb_sub, depth_sub, camera_sub)
    {
        sync.registerCallback(boost::bind(&OpenNICaptureImplRGBD::input_callback, this, _1, _2, _3));
     }


    bool grab(cv::Mat &clr, cv::Mat &depth, ros::Time &t, image_geometry::PinholeCameraModel &cm)
    {
        if ( !connected ) return false ;

        boost::unique_lock<boost::mutex> lock_ (image_lock) ;

        if ( !tmp_rgb || !tmp_depth ) return false ;

        cv_bridge::CvImagePtr rgb_ = cv_bridge::toCvCopy(tmp_rgb, enc::BGR8);
        cv_bridge::CvImagePtr depth_ = cv_bridge::toCvCopy(tmp_depth, "");

        clr = rgb_->image ;
        depth = depth_->image ;
        cm.fromCameraInfo(tmp_camera) ;

        t = tmp_depth->header.stamp ;

        if ( clr.data == NULL || depth.data == NULL ) return false ;
        else return true ;
    }


private:

    void setup()
    {
        // Subscribe to rgb and depth streams

        rgb_sub.subscribe(nh, "/" + prefix + "/rgb/image_rect_color", 10);
        depth_sub.subscribe(nh, "/" + prefix + "/depth_registered/image_rect_raw", 10);
        camera_sub.subscribe(nh, "/" + prefix + "/depth_registered/camera_info", 10);
    }

    virtual void shutdown()
    {
        rgb_sub.unsubscribe();
        depth_sub.unsubscribe();
        camera_sub.unsubscribe() ;
    }


    sensor_msgs::ImageConstPtr tmp_rgb, tmp_depth ;
    sensor_msgs::CameraInfoConstPtr tmp_camera ;
    message_filters::Subscriber<sensor_msgs::Image> rgb_sub, depth_sub ;
    message_filters::Subscriber<sensor_msgs::CameraInfo> camera_sub ;
    message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image, sensor_msgs::CameraInfo> > sync ;

    void input_callback(const ImageConstPtr& rgb, const ImageConstPtr& depth, const CameraInfoConstPtr &camera)
    {
        {
            boost::unique_lock<boost::mutex> lock_ (image_lock) ;

            // Store current images
            tmp_rgb = rgb ;
            tmp_depth = depth ;
            tmp_camera = camera ;
            dataReady = true ;
        }

        data_ready.unlock() ;
    }

};

///////////////////////////////////////////////////////////////////////////////

class OpenNICaptureImplCloud: public OpenNICaptureImpl
{
public:

    OpenNICaptureImplCloud(const string &prefix_): OpenNICaptureImpl(prefix_)
    {
        cloud_sub.registerCallback(boost::bind(&OpenNICaptureImplCloud::input_callback, this, _1));
    }


    bool grab(pcl::PointCloud<pcl::PointXYZ> &pc, ros::Time &ts)
    {
        boost::unique_lock<boost::mutex> lock_ (image_lock) ;

        if ( !connected || !tmp_cloud ) return false ;

        pcl::fromROSMsg(*tmp_cloud, pc) ;

        ts = tmp_cloud->header.stamp ;

        return true ;

    }

    bool grab(pcl::PointCloud<pcl::PointXYZRGB> &pc, ros::Time &ts)
    {
        boost::unique_lock<boost::mutex> lock_ (image_lock) ;

        if ( !connected || !tmp_cloud ) return false ;

        pcl::fromROSMsg(*tmp_cloud, pc) ;

        ts = tmp_cloud->header.stamp ;

        return true ;
    }

private:

    void setup()
    {
        // Subscribe to rgb and depth streams
        cloud_sub.subscribe(nh, "/" + prefix + "/depth_registered/points", 1);
    }

    void shutdown()
    {
        cloud_sub.unsubscribe();
    }


    void input_callback(const PointCloud2ConstPtr& cloud)
    {
        {
            boost::unique_lock<boost::mutex> lock_ (image_lock) ;

            // Store current images
            tmp_cloud = cloud ;
            dataReady = true ;

            data_ready.unlock() ;
        }
    }

    sensor_msgs::PointCloud2ConstPtr tmp_cloud ;
    message_filters::Subscriber<sensor_msgs::PointCloud2> cloud_sub ;

};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////


class OpenNICaptureImplAll: public OpenNICaptureImpl
{
public:

    OpenNICaptureImplAll(const string &prefix_): OpenNICaptureImpl(prefix_),
        sync(sync_policies::ApproximateTime<Image, Image, PointCloud2, CameraInfo>(10), rgb_sub, depth_sub, cloud_sub, camera_sub)
    {
        sync.registerCallback(boost::bind(&OpenNICaptureImplAll::input_callback, this, _1, _2, _3, _4));
    }


    bool grab(cv::Mat &clr, cv::Mat &depth, pcl::PointCloud<pcl::PointXYZ> &pc, ros::Time &ts, image_geometry::PinholeCameraModel &cm)
    {
        boost::unique_lock<boost::mutex> lock_ (image_lock) ;

        if ( !connected || !tmp_cloud || !tmp_rgb || !tmp_depth ) return false ;

        pcl::fromROSMsg(*tmp_cloud, pc) ;

        cv_bridge::CvImagePtr rgb_ = cv_bridge::toCvCopy(tmp_rgb, enc::BGR8);
        cv_bridge::CvImagePtr depth_ = cv_bridge::toCvCopy(tmp_depth, "");

        clr = rgb_->image ;
        depth = depth_->image ;
        cm.fromCameraInfo(tmp_camera) ;
        ts = tmp_cloud->header.stamp ;

        return true ;
    }

private:

    void setup()
    {
        cloud_sub.subscribe(nh, "/" + prefix + "/depth_registered/points", 10);
        rgb_sub.subscribe(nh, "/" + prefix + "/rgb/image_rect_color", 10);
        depth_sub.subscribe(nh, "/" + prefix + "/depth_registered/image_rect_raw", 10);
        camera_sub.subscribe(nh, "/" + prefix + "/depth_registered/camera_info", 10);

    }

    void shutdown()
    {
        cloud_sub.unsubscribe();
        rgb_sub.unsubscribe();
        depth_sub.unsubscribe();
        camera_sub.unsubscribe() ;
    }


    sensor_msgs::ImageConstPtr tmp_rgb, tmp_depth;
    sensor_msgs::CameraInfoConstPtr tmp_camera;
    sensor_msgs::PointCloud2ConstPtr tmp_cloud ;
    message_filters::Subscriber<sensor_msgs::Image> rgb_sub, depth_sub ;
    message_filters::Subscriber<sensor_msgs::PointCloud2> cloud_sub ;
    message_filters::Subscriber<sensor_msgs::CameraInfo> camera_sub ;
    message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image, sensor_msgs::PointCloud2, sensor_msgs::CameraInfo> > sync ;

    void input_callback(const ImageConstPtr &rgb, const ImageConstPtr &depth, const PointCloud2ConstPtr& cloud, const CameraInfoConstPtr &camera)
    {
        {
            boost::unique_lock<boost::mutex> lock_ (image_lock) ;

            // Store current images
            tmp_rgb = rgb ;
            tmp_depth = depth ;
            tmp_cloud = cloud ;
            tmp_camera = camera ;
            dataReady = true ;
        }

        data_ready.unlock() ;
    }


};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool OpenNICaptureBase::connect(ros::Duration timeout) {
    return impl_->connect(timeout) ;
}


void  OpenNICaptureBase::connect(boost::function<void ()> cb, ros::Duration timeout) {
    impl_->connect(cb, timeout) ;
}

// disconnect from the camera
void OpenNICaptureBase::disconnect() {
    impl_->disconnect();
}

bool OpenNICaptureBase::isConnected() const {
    return impl_->isConnected() ;
}

OpenNICaptureBase::~OpenNICaptureBase() {
    delete impl_ ;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

OpenNICaptureRGBD::OpenNICaptureRGBD(const string &prefix_) {
    impl_ = new OpenNICaptureImplRGBD(prefix_) ;
}

bool OpenNICaptureRGBD::grab(cv::Mat &clr, cv::Mat &depth, ros::Time &t, image_geometry::PinholeCameraModel &cm)
{
    return ((OpenNICaptureImplRGBD *)impl_)->grab(clr, depth, t, cm) ;
}


/////////////////////////////////////////////////////////////////////////////////////////


OpenNICapturePointCloud::OpenNICapturePointCloud(const string &prefix_) {
    impl_ = new OpenNICaptureImplCloud(prefix_) ;
}

bool OpenNICapturePointCloud::grab(pcl::PointCloud<pcl::PointXYZ> &pc, ros::Time &t)
{
    return ((OpenNICaptureImplCloud *)impl_)->grab(pc, t) ;
}

bool OpenNICapturePointCloud::grab(pcl::PointCloud<pcl::PointXYZRGB> &pc, ros::Time &t)
{
    return ((OpenNICaptureImplCloud *)impl_)->grab(pc, t) ;
}

/////////////////////////////////////////////////////////////////////////////////////////


OpenNICaptureAll::OpenNICaptureAll(const string &prefix_)  {
    impl_ = new OpenNICaptureImplAll(prefix_) ;
}

bool OpenNICaptureAll::grab(cv::Mat &clr, cv::Mat &depth, pcl::PointCloud<pcl::PointXYZ> &cloud, ros::Time &ts, image_geometry::PinholeCameraModel &cm)
{
    return ((OpenNICaptureImplAll *)impl_)->grab(clr, depth, cloud, ts, cm) ;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////

}
