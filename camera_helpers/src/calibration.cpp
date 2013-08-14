#include "calibration.h"

#include <highgui.h>

#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include <iostream>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>

#include <Eigen/Geometry>

using namespace std ;
using namespace cv ;
using namespace Eigen ;

static bool findCorners(const cv::Mat &im, const cv::Size &boardSize, vector<Point2f> &corners)
{
    cv::Mat gray ;

    cv::cvtColor(im, gray, CV_BGR2GRAY) ;

    //CALIB_CB_FAST_CHECK saves a lot of time on images
    //that do not contain any chessboard corners

    bool patternfound = cv::findChessboardCorners(gray, boardSize, corners,
            CALIB_CB_ADAPTIVE_THRESH + CALIB_CB_NORMALIZE_IMAGE );

    if(patternfound)
        cv::cornerSubPix(gray, corners, Size(5, 5), Size(-1, -1),
            TermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));
/*

    cv::drawChessboardCorners(im, boardSize, Mat(corners), patternfound);

    cv::imwrite("/home/malasiot/tmp/oo.png", im) ;

    cout << "ok here" ;
*/

    return patternfound ;
}

static double computeReprojectionErrors(
        const vector<vector<Point3f> >& objectPoints,
        const vector<vector<Point2f> >& imagePoints,
        const vector<Mat>& rvecs, const vector<Mat>& tvecs,
        const Mat& cameraMatrix, const Mat& distCoeffs,
        vector<float>& perViewErrors )
{
    vector<Point2f> imagePoints2;
    int i, totalPoints = 0;
    double totalErr = 0, err;
    perViewErrors.resize(objectPoints.size());

    for( i = 0; i < (int)objectPoints.size(); i++ )
    {

        projectPoints(Mat(objectPoints[i]), rvecs[i], tvecs[i],
                      cameraMatrix, distCoeffs, imagePoints2);
        err = norm(Mat(imagePoints[i]), Mat(imagePoints2), CV_L2);
        int n = (int)objectPoints[i].size();
        perViewErrors[i] = (float)std::sqrt(err*err/n);
        totalErr += err*err;
        totalPoints += n;
    }

    return std::sqrt(totalErr/totalPoints);
}

static bool runCalibration( const vector<vector<Point2f> > &imagePoints,
                            const vector<vector<Point3f> > &objectPoints,
                            const cv::Size &imageSize,
                            float aspectRatio,
                            int flags, Mat& cameraMatrix, Mat& distCoeffs,
                            vector<Mat>& rvecs, vector<Mat>& tvecs,
                            vector<float>& reprojErrs,
                            double& totalAvgErr)
{
    cameraMatrix = Mat::eye(3, 3, CV_64F);

  //  if( flags & CV_CALIB_FIX_ASPECT_RATIO )
  //      cameraMatrix.at<double>(0,0) = aspectRatio;

    cameraMatrix.at<double>(0, 0) = 525 ;
    cameraMatrix.at<double>(1, 1) = 525 ;
    cameraMatrix.at<double>(0, 2) = imageSize.width/2 - 0.5 ;
    cameraMatrix.at<double>(1, 2) = imageSize.height/2 - 0.5 ;

    distCoeffs = Mat::zeros(8, 1, CV_64F);

    double rms = cv::calibrateCamera(objectPoints, imagePoints, imageSize, cameraMatrix,
                    distCoeffs, rvecs, tvecs, flags|CV_CALIB_FIX_K4|CV_CALIB_FIX_K5);
                    ///*|CV_CALIB_FIX_K3*/|CV_CALIB_FIX_K4|CV_CALIB_FIX_K5);
    printf("RMS error reported by calibrateCamera: %g\n", rms);

    bool ok = cv::checkRange(cameraMatrix) && cv::checkRange(distCoeffs);

    totalAvgErr = computeReprojectionErrors(objectPoints, imagePoints,
                rvecs, tvecs, cameraMatrix, distCoeffs, reprojErrs);

    return ok;
}


Affine3d randomAffine(double scale=0.1)
{

    Vector3d trans = 0.1 * Vector3d::Random() ;
    Vector4d rot = Vector4d::Random()*scale ;
    rot.normalize() ;

    return Translation3d(trans) * Quaterniond(rot).toRotationMatrix() ;
}

void find_target_motions(const string &filePrefix, const string &dataFolder, const cv::Size boardSize, const double squareSize,
                           vector<Affine3d> &gripper_to_base, vector<Affine3d> &target_to_sensor )
{
    string fileSuffix = "_c.*" ;

//    const cv::Size boardSize(3, 5) ;
//    const double squareSize = 0.04 ;

    namespace fs = boost::filesystem  ;

    std::string fileNameRegex = filePrefix + "([[:digit:]]+)" + fileSuffix ;

    boost::regex rx(fileNameRegex) ;

    vector<vector<Point2f> > img_corners ;
    vector<vector<Point3f> > obj_corners ;


    fs::directory_iterator it(dataFolder), end ;

    for( ; it != end ; ++it)
    {
        fs::path p = (*it).path() ;

        string fileName = p.filename().string() ;

        boost::smatch sm ;

        // test of this is a valid filename

        if ( !boost::regex_match(fileName, sm, rx ) ) continue ;

        std::string imageNumber = sm[1] ;

        cv::Mat im = imread(p.string(), -1) ;

        Affine3d tr ;

        {
            boost::replace_all(fileName,"_c.png", "_pose.txt") ;

            string filePath = p.parent_path().string() + '/' + fileName ;
            ifstream strm(filePath.c_str()) ;

            Matrix3d r ;
            Vector3d t ;

            double vals[12] ;

            for(int i=0 ; i<12 ; i++ ) strm >> vals[i] ;

            r << vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7], vals[8] ;
            t << vals[9], vals[10], vals[11] ;

            tr = Translation3d(t) * r ;
        }


        vector<Point2f> img_corners_ ;
        vector<Point3f> obj_corners_ ;

        img_corners.push_back(vector<Point2f>()) ;
        obj_corners.push_back(vector<Point3f>()) ;

        if ( findCorners(im, boardSize, img_corners.back()) )
        {
            for( int i = 0; i < boardSize.height; i++ )
                for( int j = 0; j < boardSize.width; j++ )
                    obj_corners.back().push_back(Point3f(float(j*squareSize), float(i*squareSize), 0));

            gripper_to_base.push_back(tr.inverse()) ;
        }
        else
        {
            img_corners.pop_back() ;
            obj_corners.pop_back() ;

        }

    }



    vector<Mat> rvecs, tvecs;
    vector<float> reprojErrs;


    double totalAvgErr = 0;

    cv::Mat camMatrix, distCoeffs ;

    int flag = CALIB_USE_INTRINSIC_GUESS | CALIB_FIX_FOCAL_LENGTH | CALIB_FIX_PRINCIPAL_POINT ;
    runCalibration(img_corners, obj_corners, cv::Size(640, 480), 640/480.0, flag,
                   camMatrix, distCoeffs, rvecs, tvecs, reprojErrs, totalAvgErr) ;


    Affine3d sensor_to_base, target_to_gripper ;

    sensor_to_base = randomAffine() ;

    cout << sensor_to_base.matrix() << endl ;
    sensor_to_base.translation() = Vector3d(0, 1.5, 0.2) ;

    target_to_gripper = randomAffine() ;

    for(unsigned int i=0 ; i<tvecs.size() ; i++ )
    {
        Matrix3d r ;
        Vector3d t ;

        cv::Mat rmat_ ;
        cv::Rodrigues(rvecs[i], rmat_) ;

        cv::Mat_<double> rmat(rmat_) ;
        cv::Mat_<double> tmat(tvecs[i]) ;

        r << rmat(0, 0), rmat(0, 1), rmat(0, 2), rmat(1, 0), rmat(1, 1), rmat(1, 2), rmat(2, 0), rmat(2, 1), rmat(2, 2) ;
        t << tmat(0, 0), tmat(0, 1), tmat(0, 2) ;

        Affine3d tr = Translation3d(t) * r ;

   //     target_to_sensor.push_back(tr) ;

        Affine3d tr_ = sensor_to_base.inverse() * gripper_to_base[i] * target_to_gripper;

        Quaterniond qq ;

        qq.vec() += Vector3d::Random()*0.001 ;
        qq.normalize();

        tr_.rotate(qq.toRotationMatrix()) ;

        target_to_sensor.push_back(tr_) ;
    }


}
