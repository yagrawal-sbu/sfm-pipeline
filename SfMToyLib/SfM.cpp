/*
 * SfM.cpp
 *
 *  Created on: May 26, 2016
 *      Author: roy_shilkrot
 * 
 *  Copyright @ Roy Shilkrot 2016
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

#include "SfM.h"
#include "SfMStereoUtilities.h"
#include "SfMBundleAdjustmentUtils.h"

#include <iostream>
#include <algorithm>
#include <thread>
#include <mutex>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/filesystem.hpp>

using namespace std;
using namespace cv;

namespace sfmtoylib {

const float MERGE_CLOUD_POINT_MIN_MATCH_DISTANCE   = 0.01;
const float MERGE_CLOUD_FEATURE_MIN_MATCH_DISTANCE = 20.0;
const int   MIN_POINT_COUNT_FOR_HOMOGRAPHY         = 100;

double findCameraDistance(cv::Matx34f& cameraPose1, cv::Matx34f& cameraPose2) {
    // Calculate distance
    
    double x = double (cameraPose1(0,3) - cameraPose2(0,3));
    double y = double (cameraPose1(1,3) - cameraPose2(1,3));
    double z = double (cameraPose1(2,3) - cameraPose2(2,3));
    
    double mySqr = x*x + y*y + z*z;
    
    double myDistance = sqrt(mySqr);
    cout << endl << "distance b/w cameras= " << myDistance << endl;
    return myDistance;
}

void print_here(int n) {
    cout << "here" << to_string(n) << endl;
}

void log_message_imp(string s) {
    cout << endl <<
    "----------------------------------------------------" << endl;
    cout << "----------------- "<< s << " -----------------" << endl;
    cout << "----------------------------------------------------" << endl;
}

SfM::SfM(const float downscale) :
        mConsoleDebugLevel(LOG_INFO),
        mVisualDebugLevel(LOG_INFO),
        mDownscaleFactor(downscale) {
}

SfM::~SfM() {
}

void dump(std::vector<Pose>& cameraPoses) {
    for (size_t i = 0; i < cameraPoses.size(); i++) {
        cout << cameraPoses[i] << endl;
    }
}

ErrorCode SfM::runSfM() {
    if (mImages.size() <= 0) {
        cerr << "No images to work on." << endl;
        return ErrorCode::ERROR;
    }

    //initialize intrinsics
    //   temple
    //   mIntrinsics.K = (Mat_<float>(3,3) << 800,    0, 400,
    //                                            0, 800, 225,
    //                                            0,    0, 1);
    //  street
    mIntrinsics.K = (Mat_<float>(3,3) <<    600,    0,      200,
                                            0,      600,    267,
                                            0,      0,      1);

    mIntrinsics.Kinv = mIntrinsics.K.inv();
    mIntrinsics.distortion = Mat_<float>::zeros(1, 4);
    // have enough in cameraPoses
    mCameraPoses.resize(mImages.size());

    //First - extract features from all images
    extractFeatures();

    //Create a matching matrix between all sequential pair of images
    createFeatureMatchMatrix();

    bool success = firstSet();
    if(!success)
        return OKAY;

    const size_t numImages = mImages.size();
    for (size_t i = 1; i < numImages; i++) {
        addView(i);
    }

    return OKAY;


    //Find the best two views for an initial triangulation on the 3D map
    findBaselineTriangulation();

    //Lastly - add more camera views to the map
    addMoreViewsToReconstruction();

    if (mConsoleDebugLevel <= LOG_INFO) {
        log_message_imp("Done");
    }

    return OKAY;
}

bool SfM::firstSet() {
    size_t i = 0;
    cout << endl << endl << "Matching pairs - ["<<i<<","<<i+1<<"]" << endl;
    // step 1
    // findBaselineTriangulation
    // step 1.1
    // sortViewsForBaseline
    if (mFeatureMatchMatrix[i][i+1].size() < MIN_POINT_COUNT_FOR_HOMOGRAPHY) {
        cout <<  "Not enough points for homography." << endl;
        return false;
    }
    //Find number of homography inliers
    const int numInliers = SfMStereoUtilities::findHomographyInliers(
                                                                     mImageFeatures[i],
                                                                     mImageFeatures[i+1],
                                                                     mFeatureMatchMatrix[i][i+1]);
    const float inliersRatio = (float)numInliers / (float)(mFeatureMatchMatrix[i][i+1].size());
    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << "Homography inliers ratio: " << inliersRatio << endl;
    }
    // step 1.2
    // findCameraMatricesFromMatch
    Matx34f Pleft  = Matx34f::eye();
    Matx34f Pright = Matx34f::eye();
    PointCloud pointCloud;
    Matching prunedMatching;
    //recover camera matrices (poses) from the point matching
    bool success = SfMStereoUtilities::findCameraMatricesFromMatch(
                                                                   mIntrinsics,
                                                                   mFeatureMatchMatrix[i][i+1],
                                                                   mImageFeatures[i],
                                                                   mImageFeatures[i+1],
                                                                   prunedMatching,
                                                                   Pleft, Pright
                                                                   );
    
    if (not success) {
        if (mConsoleDebugLevel <= LOG_WARN) {
            cerr << "stereo view could not be obtained "  << i << ", " << i+1 <<  ", go to next pair" << endl << flush;
        }
        return false;
    }
    
    float poseInliersRatio = (float)prunedMatching.size() / (float)mFeatureMatchMatrix[i][i+1].size();
    
    if (mConsoleDebugLevel <= LOG_TRACE) {
        cout << "Pose inliers ratio: " << poseInliersRatio << endl;
    }
    
    if (poseInliersRatio < POSE_INLIERS_MINIMAL_RATIO) {
        if (mConsoleDebugLevel <= LOG_TRACE) {
            cout << "insufficient pose inliers. skip." << endl;
        }
        return false;
    }
    if (mVisualDebugLevel <= LOG_INFO) {
        Mat outImage;
        drawMatches(mImages[i], mImageFeatures[i].keyPoints,
                    mImages[i+1], mImageFeatures[i+1].keyPoints,
                    prunedMatching,
                    outImage);
        // resize(outImage, outImage, Size(), 0.5, 0.5);
        imshow("outimage", outImage);
        waitKey(0);
        destroyWindow("outimage");
    }
    
    mFeatureMatchMatrix[i][i+1] = prunedMatching;
    
    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << "Triangulate stereo views" << endl;
    }
    ImagePair imagePair = {i, i+1};
    success = SfMStereoUtilities::triangulateViews(
                                                   mIntrinsics,
                                                   imagePair,
                                                   mFeatureMatchMatrix[i][i+1],
                                                   mImageFeatures[i], mImageFeatures[i+1],
                                                   Pleft, Pright,
                                                   pointCloud
                                                   );
    if (not success) {
        if (mConsoleDebugLevel <= LOG_WARN) {
            cerr << "could not triangulate: " << imagePair << endl << flush;
        }
        return false;
    }
    
    cout << "pointCloud size=" << pointCloud.size() << endl;
    mReconstructionCloud = pointCloud;
    mCameraPoses[i] = Pleft;
    mCameraPoses[i+1] = Pright;
//    adjustCurrentBundleW(10);
    mlinearDistances.push_back( findCameraDistance(mCameraPoses[i], mCameraPoses[i+1]) );

    return true;
}


bool SfM::setImagesDirectory(const std::string& directoryPath) {
    using namespace boost::filesystem;

    path dirPath(directoryPath);
    if (not exists(dirPath) or not is_directory(dirPath)) {
        cerr << "Cannot open directory: " << directoryPath << endl;
        return false;
    }

    for (directory_entry& x : directory_iterator(dirPath)) {
        string extension = x.path().extension().string();
        boost::algorithm::to_lower(extension);
        if (extension == ".jpg" or extension == ".png") {
            // cout << x.path().string() << endl; // line to print the images to be read
            mImageFilenames.push_back(x.path().string());
        }
    }

    if (mImageFilenames.size() <= 0) {
        cerr << "Unable to find valid files in images directory (\"" << directoryPath << "\")." << endl;
        return false;
    }

    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << "Found " << mImageFilenames.size() << " image files in directory." << endl;
    }

    for (auto& imageFilename : mImageFilenames) {
        mImages.push_back(imread(imageFilename));

        if (mDownscaleFactor != 1.0) {
            resize(mImages.back(), mImages.back(), Size(), mDownscaleFactor, mDownscaleFactor);
        }

        if (mImages.back().empty()) {
            cerr << "Unable to read image from file: " << imageFilename << endl;
            return false;
        }
    }

    return true;
}


void SfM::extractFeatures() {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << endl <<
                "----------------------------------------------------" << endl;
        cout << "----------------- Extract Features -----------------" << endl;
        cout << "----------------------------------------------------" << endl;
    }

    mImageFeatures.resize(mImages.size());
    for (size_t i = 0; i < mImages.size(); i++) {
        mImageFeatures[i] = mFeatureUtil.extractFeatures(mImages[i]);

        if (mConsoleDebugLevel <= LOG_DEBUG) {
            cout << "Image " << i << ": " << mImageFeatures[i].keyPoints.size() << " keypoints" << endl;
        }
    }
}


void SfM::createFeatureMatchMatrix() {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << endl <<
                "----------------------------------------------------" << endl;
        cout << "----------- Create Feature Match Matrix ------------" << endl;
        cout << "----------------------------------------------------" << endl;
    }

    const size_t numImages = mImages.size();
    mFeatureMatchMatrix.resize(numImages, vector<Matching>(numImages));

    //prepare image pairs to go as per the sequence
    vector<ImagePair> pairs;
    for (size_t i = 0; i < numImages-1; i++) {
//        for (size_t j = i+1; j < numImages; j++) {
//            pairs.push_back({ i, j });
//        }
        pairs.push_back({ i, i+1 });
    }

    vector<thread> threads;

    //find out how many threads are supported, and how many pairs each thread will work on
    const int numThreads = std::thread::hardware_concurrency() - 1;
    const int numPairsForThread = (numThreads > pairs.size()) ? 1 : (int)ceilf((float)(pairs.size()) / numThreads);

    mutex writeMutex;

    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << "Launch " << numThreads << " threads with " << numPairsForThread << " pairs per thread" << endl;
    }

    //invoke each thread with its pairs to process (if less pairs than threads, invoke only #pairs threads with 1 pair each)
    for (size_t threadId = 0; threadId < MIN(numThreads, pairs.size()); threadId++) {
        threads.push_back(thread([&, threadId] {
            const int startingPair = numPairsForThread * threadId;

            for (int j = 0; j < numPairsForThread; j++) {
                const int pairId = startingPair + j;
                if (pairId >= pairs.size()) { //make sure threads don't overflow the pairs
                    break;
                }
                const ImagePair& pair = pairs[pairId];

                mFeatureMatchMatrix[pair.left][pair.right] = SfM2DFeatureUtilities::matchFeatures(mImageFeatures[pair.left], mImageFeatures[pair.right]);

                if (mConsoleDebugLevel <= LOG_DEBUG) {
                    writeMutex.lock();
                    cout << "Thread " << threadId << ": Match (pair " << pairId << ") " << pair.left << ", " << pair.right << ": " << mFeatureMatchMatrix[pair.left][pair.right].size() << " matched features" << endl;
                    writeMutex.unlock();
                }
            }
        }));
    }

    //wait for threads to complete
    for (auto& t : threads) {
        t.join();
    }
}


void SfM::findBaselineTriangulation() {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << endl <<
                "----------------------------------------------------" << endl;
        cout << "----------- Find Baseline Triangulation ------------" << endl;
        cout << "----------------------------------------------------" << endl;
    }

    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << endl << "--- Sort views by homography inliers" << endl;
    }

    //maps are sorted, so the best pair is at the beginnning
    map<float, ImagePair> pairsHomographyInliers = sortViewsForBaseline();

    Matx34f Pleft  = Matx34f::eye();
    Matx34f Pright = Matx34f::eye();
    PointCloud pointCloud;

    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << endl << "--- Try views in triangulation" << endl;
    }

    //try to find the best pair, stating at the beginning
    for (auto& imagePair : pairsHomographyInliers) {

        if (mConsoleDebugLevel <= LOG_DEBUG) {
            cout << "Trying " << imagePair.second << " ratio: " << imagePair.first << endl << flush;
        }

        //GO_IN_SEQUENCE_CHANGE
        if(imagePair.second.left != 0 && imagePair.second.right != 1)
            continue;


        size_t i = imagePair.second.left;
        size_t j = imagePair.second.right;


        if (mConsoleDebugLevel <= LOG_TRACE) {
            cout << "---- Find camera matrices" << endl;
        }

        Matching prunedMatching;
        //recover camera matrices (poses) from the point matching
        bool success = SfMStereoUtilities::findCameraMatricesFromMatch(
                mIntrinsics,
                mFeatureMatchMatrix[i][j],
                mImageFeatures[i],
                mImageFeatures[j],
				prunedMatching,
                Pleft, Pright
                );

        if (not success) {
            if (mConsoleDebugLevel <= LOG_WARN) {
                cerr << "stereo view could not be obtained " << imagePair.second << ", go to next pair" << endl << flush;
            }
            continue;
        }

        float poseInliersRatio = (float)prunedMatching.size() / (float)mFeatureMatchMatrix[i][j].size();

        if (mConsoleDebugLevel <= LOG_TRACE) {
            cout << "pose inliers ratio " << poseInliersRatio << endl;
        }

        if (poseInliersRatio < POSE_INLIERS_MINIMAL_RATIO) {
            if (mConsoleDebugLevel <= LOG_TRACE) {
                cout << "insufficient pose inliers. skip." << endl;
            }
            continue;
        }

        if (mVisualDebugLevel <= LOG_INFO) {
            Mat outImage;
            drawMatches(mImages[i], mImageFeatures[i].keyPoints,
                        mImages[j], mImageFeatures[j].keyPoints,
                        prunedMatching,
                        outImage);
            resize(outImage, outImage, Size(), 0.5, 0.5);
            imshow("outimage", outImage);
            waitKey(0);
            destroyWindow("outimage");
        }

        mFeatureMatchMatrix[i][j] = prunedMatching;

        if (mConsoleDebugLevel <= LOG_DEBUG) {
            cout << "---- Triangulate from stereo views: " << imagePair.second << endl;
        }

//        cout << "mIntrinsics=" << endl << mIntrinsics << endl;
//        cout << endl << "imagePair.second=" << endl << imagePair.second << endl;
//        cout << "mFeatureMatchMatrix[i][j]" << endl << mFeatureMatchMatrix[i][j] << endl;
//        cout << "Pleft=" << endl << Pleft << endl;
//        cout << "Pright=" << endl << Pright << endl;
//        cout << "pointCloud=" << endl << pointCloud << endl;

        success = SfMStereoUtilities::triangulateViews(
                mIntrinsics,
                imagePair.second,
                mFeatureMatchMatrix[i][j],
                mImageFeatures[i], mImageFeatures[j],
                Pleft, Pright,
                pointCloud
                );

       if (not success) {
           if (mConsoleDebugLevel <= LOG_WARN) {
               cerr << "could not triangulate: " << imagePair.second << endl << flush;
           }
           continue;
       }

       cout << "pointCloud size=" << pointCloud.size() << endl;
       mReconstructionCloud = pointCloud;
       mCameraPoses[i] = Pleft;
       mCameraPoses[j] = Pright;
       findCameraDistance(mCameraPoses[i], mCameraPoses[j]);
       mDoneViews.insert(i);
       mDoneViews.insert(j);
       mGoodViews.insert(i);
       mGoodViews.insert(j);

        dump(mCameraPoses);
       adjustCurrentBundle();
        dump(mCameraPoses);
       break;
    }
}


void SfM::adjustCurrentBundle() {
    SfMBundleAdjustmentUtils::adjustBundle(
            mReconstructionCloud,
            mCameraPoses,
            mIntrinsics,
            mImageFeatures);
}

void SfM::adjustCurrentBundleW(int w) {
    SfMBundleAdjustmentUtils::adjustBundleWeight(
                                        mReconstructionCloud,
                                        mCameraPoses,
                                        mIntrinsics,
                                        mImageFeatures,
                                        w);
}


map<float, ImagePair> SfM::sortViewsForBaseline() {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << "---------- Find Views Homography Inliers -----------" << endl;
    }

    //sort pairwise matches to find the lowest Homography inliers [Snavely07 4.2]
    map<float, ImagePair> matchesSizes;
    const size_t numImages = mImages.size();
    for (size_t i = 0; i < numImages - 1; i++) {
        for (size_t j = i + 1; j < numImages; j++) {
            if (mFeatureMatchMatrix[i][j].size() < MIN_POINT_COUNT_FOR_HOMOGRAPHY) {
                //Not enough points in matching
//                matchesSizes[1.0] = {i, j};
                continue;
            }

            //Find number of homography inliers
            const int numInliers = SfMStereoUtilities::findHomographyInliers(
                    mImageFeatures[i],
                    mImageFeatures[j],
                    mFeatureMatchMatrix[i][j]);
            const float inliersRatio = (float)numInliers / (float)(mFeatureMatchMatrix[i][j].size());
            matchesSizes[inliersRatio] = {i, j};

            if (mConsoleDebugLevel <= LOG_DEBUG) {
                cout << "Homography inliers ratio: " << i << ", " << j << " " << inliersRatio << endl;
            }
        }
    }
    if (mConsoleDebugLevel <= LOG_DEBUG)
        cout << "matchesSizes has " << matchesSizes.size() << " elements"<< endl;
    return matchesSizes;
}


void SfM::addView(size_t viewIdx) {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << endl <<
                "----------------------------------------------------" << endl;
        cout << "------------------ Add View " << viewIdx <<"--------------------" << endl;
        cout << "----------------------------------------------------" << endl;
    }
    // find2D3DMatches
    Image2D3DMatch match2D3D;
    //scan all cloud 3D points
    for (const Point3DInMap& cloudPoint : mReconstructionCloud) {
        bool found2DPoint = false;
        
//        cout << endl << "cloudpoint= " << cloudPoint.p << ", originating= " << cloudPoint.originatingViews.size() << endl;

        //scan all originating views for that 3D point
        for (const auto& origViewAndPoint : cloudPoint.originatingViews) {
            //check for 2D-2D matching via the match matrix
            const int originatingViewIndex        = origViewAndPoint.first;
            const int originatingViewFeatureIndex = origViewAndPoint.second;
            if(originatingViewIndex!=viewIdx-1)
                continue;
            
            //match matrix is upper-triangular (not symmetric) so the left index must be the smaller one
            const int leftViewIdx  = (originatingViewIndex < viewIdx) ? originatingViewIndex : viewIdx;
            const int rightViewIdx = (originatingViewIndex < viewIdx) ? viewIdx : originatingViewIndex;
//            cout << leftViewIdx << rightViewIdx << endl;
            //scan all 2D-2D matches between originating view and new view
            for (const DMatch& m : mFeatureMatchMatrix[leftViewIdx][rightViewIdx]) {
                int matched2DPointInNewView = -1;
                if (originatingViewIndex < viewIdx) { //originating view is 'left'
                    if (m.queryIdx == originatingViewFeatureIndex) {
//                        cout << "oVFI=" << originatingViewFeatureIndex << endl;
                        matched2DPointInNewView = m.trainIdx;
                    }
                } else {                              //originating view is 'right'
                    if (m.trainIdx == originatingViewFeatureIndex) {
                        matched2DPointInNewView = m.queryIdx;
                    }
                }
                if (matched2DPointInNewView >= 0) {
                    //This point is matched in the new view
                    const Features& newViewFeatures = mImageFeatures[viewIdx];
                    match2D3D.points2D.push_back(newViewFeatures.points[matched2DPointInNewView]);
                    match2D3D.points3D.push_back(cloudPoint.p);
                    found2DPoint = true;
                    break;
                }
            }
            
            if (found2DPoint) {
                break;
            }
        }
    }
    //recover the new view camera pose
    Matx34f newCameraPose;
    bool success = SfMStereoUtilities::findCameraPoseFrom2D3DMatch(
                                                                   mIntrinsics,
                                                                   match2D3D,
                                                                   newCameraPose);
    if (not success) {
        if (mConsoleDebugLevel <= LOG_WARN) {
            cerr << "Cannot recover camera pose for view " << viewIdx << endl;
        }
        return;
    }
    mCameraPoses[viewIdx] = newCameraPose;
    // dump(mCameraPoses);

    //triangulate more points from new view to all existing good views
    bool anyViewSuccess = false;
    int bestView = viewIdx;
    for (int goodView=bestView-1; goodView<bestView; goodView++) {
        //since match matrix is upper-triangular (non symmetric) - use lower index as left
        size_t leftViewIdx  = (goodView < bestView) ? goodView : bestView;
        size_t rightViewIdx = (goodView < bestView) ? bestView : goodView;
        
        Matching prunedMatching;
        Matx34f Pleft  = Matx34f::eye();
        Matx34f Pright = Matx34f::eye();

        //use the essential matrix recovery to prune the matches
        bool success = SfMStereoUtilities::findCameraMatricesFromMatch(
                                                                       mIntrinsics,
                                                                       mFeatureMatchMatrix[leftViewIdx][rightViewIdx],
                                                                       mImageFeatures[leftViewIdx],
                                                                       mImageFeatures[rightViewIdx],
                                                                       prunedMatching,
                                                                       Pleft, Pright
                                                                       );
        cout << Pleft << endl;
        cout << Pright << endl;
        mFeatureMatchMatrix[leftViewIdx][rightViewIdx] = prunedMatching;
        
        Pright(0,3) += mCameraPoses[viewIdx-1](0,3);
        Pright(1,3) += mCameraPoses[viewIdx-1](1,3);
        Pright(2,3) += mCameraPoses[viewIdx-1](2,3);
        // add rotation as well
//        Mat r1  = Mat::zeros(cv::Size(3,3), CV_64FC1);
//        for(int i=0; i<3; i++) {
//            for(int j=0; j<3; j++) {
//                r1.at<float>(i,j) = mCameraPoses[viewIdx-1](i,j);
//            }
//        }
//        Mat r2  = Mat::zeros(cv::Size(3,3), CV_64FC1);
//        for(int i=0; i<3; i++) {
//            for(int j=0; j<3; j++) {
//                r2.at<float>(i,j) = Pright(i,j);
//            }
//        }
//        cout << r1 << r2 << endl;
//        r1 = r1 * r2;
//        for(int i=0; i<3; i++) {
//            for(int j=0; j<3; j++) {
//                Pright(i,j) = r1.at<float>(i,j);
//            }
//        }
        mCameraPoses[viewIdx] = Pright;
        //triangulate the matching points
        PointCloud pointCloud;
        
        success = SfMStereoUtilities::triangulateViews(
                                                       mIntrinsics,
                                                       { leftViewIdx, rightViewIdx },
                                                       mFeatureMatchMatrix[leftViewIdx][rightViewIdx],
                                                       mImageFeatures[leftViewIdx], mImageFeatures[rightViewIdx],
//                                                       mCameraPoses[leftViewIdx], mCameraPoses[rightViewIdx],
                                                       Pleft, Pright,
                                                       pointCloud
                                                       );

        if (success) {
            if (mConsoleDebugLevel <= LOG_DEBUG) {
                cout << "Merge triangulation between " << leftViewIdx << " and " << rightViewIdx <<
                " (# matching pts = " << (mFeatureMatchMatrix[leftViewIdx][rightViewIdx].size()) << ") ";
            }
            //add new points to the reconstruction
            cout << "new pointCloud=" << pointCloud.size() << endl;
            mergeNewPointCloud(pointCloud);
            cout << "new size of the point cloud=" << mReconstructionCloud.size() << endl;
            
            anyViewSuccess = true;
        } else {
            if (mConsoleDebugLevel <= LOG_WARN) {
                cerr << "Failed to triangulate " << leftViewIdx << " and " << rightViewIdx << endl;
            }
        }

    }
    
    //Adjust bundle if any additional view was added
    if (anyViewSuccess) {
        int w = 1;
        if(viewIdx%4==0)
            w = 1;
//        adjustCurrentBundleW(w);
        mlinearDistances.push_back( findCameraDistance(mCameraPoses[viewIdx], mCameraPoses[0]) );
    }
}

void SfM::addMoreViewsToReconstruction() {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << endl <<
                "----------------------------------------------------" << endl;
        cout << "------------------ Add More Views ------------------" << endl;
        cout << "----------------------------------------------------" << endl;
    }

    while (mDoneViews.size() != mImages.size()) {
        //Find the best view to add, according to the largest number of 2D-3D corresponding points
        Images2D3DMatches matches2D3D = find2D3DMatches();
        size_t bestView;
        size_t bestNumMatches = 0;
//        size_t left = 0;
//        size_t right = 0;
        for (const auto& match2D3D : matches2D3D) {
			const size_t numMatches = match2D3D.second.points2D.size();
			if (numMatches > bestNumMatches) {
                bestView       = match2D3D.first;
//                left           = match2D3D.second.left;
//                right           = match2D3D.second.right;
                bestNumMatches = numMatches;
            }
        }
        if (mConsoleDebugLevel <= LOG_DEBUG) {
            cout << endl << "Best view pair " << bestView  << " has " << bestNumMatches << " matches" << endl;
            cout << "Adding " << bestView << " to existing " << Mat(vector<int>(mGoodViews.begin(), mGoodViews.end())).t() << endl;
        }
        mDoneViews.insert(bestView);
        //recover the new view camera pose
        Matx34f newCameraPose;
        bool success = SfMStereoUtilities::findCameraPoseFrom2D3DMatch(
                mIntrinsics,
                matches2D3D[bestView],
                newCameraPose);

        if (not success) {
            if (mConsoleDebugLevel <= LOG_WARN) {
                cerr << "Cannot recover camera pose for view " << bestView << endl;
            }
            continue;
        }
        mCameraPoses[bestView] = newCameraPose;

        if (mConsoleDebugLevel <= LOG_DEBUG) {
            cout << "New view " << bestView << " pose " << endl;
            // cout << newCameraPose << endl;
        }
        //triangulate more points from new view to all existing good views
        bool anyViewSuccess = false;
        for (const int goodView : mGoodViews) {
            //GO_IN_SEQUENCE_CHANGE
            if(goodView!=bestView+1 && goodView!=bestView-1)
                continue;

//            print_here(1);
//            cout << "goodView=" << goodView << endl;
//            cout << "bestView=" << bestView << endl;
            //since match matrix is upper-triangular (non symmetric) - use lower index as left
            size_t leftViewIdx  = (goodView < bestView) ? goodView : bestView;
            size_t rightViewIdx = (goodView < bestView) ? bestView : goodView;

            Matching prunedMatching;
            Matx34f Pleft  = Matx34f::eye();
            Matx34f Pright = Matx34f::eye();

//            print_here(2);
//            cout << "mIntrinsics= " << mIntrinsics.K << endl;
//            cout << "left mImageFeatures= " << mImageFeatures[leftViewIdx].points.size() << endl;
//            cout << "right mImageFeatures= " << mImageFeatures[rightViewIdx].points.size() << endl;
//            cout << "mFeatureMatchMatrix= " << mFeatureMatchMatrix[leftViewIdx][rightViewIdx].size() << endl;
            //use the essential matrix recovery to prune the matches
            cout << leftViewIdx << rightViewIdx << endl;
            bool success = SfMStereoUtilities::findCameraMatricesFromMatch(
                    mIntrinsics,
                    mFeatureMatchMatrix[leftViewIdx][rightViewIdx],
                    mImageFeatures[leftViewIdx],
                    mImageFeatures[rightViewIdx],
    				prunedMatching,
                    Pleft, Pright
                    );
            mFeatureMatchMatrix[leftViewIdx][rightViewIdx] = prunedMatching;
//            print_here(3);
            //triangulate the matching points
            PointCloud pointCloud;
            
//            cout << "mIntrinsics=" << endl << mIntrinsics << endl;
//            cout << "imagePair.second=" << endl << imagePair.second << endl;
//            cout << "mFeatureMatchMatrix[i][j]" << endl << mFeatureMatchMatrix[i][j] << endl;
//            cout << "leftViewIdx=" << endl << leftViewIdx << endl;
//            cout << "rightViewIdx=" << endl << rightViewIdx << endl;
//            cout << "pointCloud=" << endl << pointCloud << endl;
//            print_here(4);

            success = SfMStereoUtilities::triangulateViews(
                    mIntrinsics,
                    { leftViewIdx, rightViewIdx },
                    mFeatureMatchMatrix[leftViewIdx][rightViewIdx],
                    mImageFeatures[leftViewIdx], mImageFeatures[rightViewIdx],
                    mCameraPoses[leftViewIdx], mCameraPoses[rightViewIdx],
                    pointCloud
                    );

//            print_here(5);
            if (success) {
                if (mConsoleDebugLevel <= LOG_DEBUG) {
                    cout << "Merge triangulation between " << leftViewIdx << " and " << rightViewIdx <<
                        " (# matching pts = " << (mFeatureMatchMatrix[leftViewIdx][rightViewIdx].size()) << ") ";
                }

                //add new points to the reconstruction
                mergeNewPointCloud(pointCloud);

                anyViewSuccess = true;
            } else {
                if (mConsoleDebugLevel <= LOG_WARN) {
                    cerr << "Failed to triangulate " << leftViewIdx << " and " << rightViewIdx << endl;
                }
            }
//            print_here(6);
        }

        //Adjust bundle if any additional view was added
        if (anyViewSuccess) {
            adjustCurrentBundle();
        }
        mGoodViews.insert(bestView);
    }
}


SfM::Images2D3DMatches SfM::find2D3DMatches() {
    Images2D3DMatches matches;

    //scan all not-done views
    for (size_t viewIdx = 0; viewIdx < mImages.size(); viewIdx++) {
        if (mDoneViews.find(viewIdx) != mDoneViews.end()) {
            continue; //skip done views
        }

        Image2D3DMatch match2D3D;

        //scan all cloud 3D points
        for (const Point3DInMap& cloudPoint : mReconstructionCloud) {
        	bool found2DPoint = false;

            //scan all originating views for that 3D point
            for (const auto& origViewAndPoint : cloudPoint.originatingViews) {
                //check for 2D-2D matching via the match matrix
                const int originatingViewIndex        = origViewAndPoint.first;
                const int originatingViewFeatureIndex = origViewAndPoint.second;

                //match matrix is upper-triangular (not symmetric) so the left index must be the smaller one
                const int leftViewIdx  = (originatingViewIndex < viewIdx) ? originatingViewIndex : viewIdx;
                const int rightViewIdx = (originatingViewIndex < viewIdx) ? viewIdx : originatingViewIndex;

                //scan all 2D-2D matches between originating view and new view
                for (const DMatch& m : mFeatureMatchMatrix[leftViewIdx][rightViewIdx]) {
                    int matched2DPointInNewView = -1;
                    if (originatingViewIndex < viewIdx) { //originating view is 'left'
                        if (m.queryIdx == originatingViewFeatureIndex) {
                            matched2DPointInNewView = m.trainIdx;
                        }
                    } else {                              //originating view is 'right'
                        if (m.trainIdx == originatingViewFeatureIndex) {
                            matched2DPointInNewView = m.queryIdx;
                        }
                    }
                    if (matched2DPointInNewView >= 0) {
                        //This point is matched in the new view
                        const Features& newViewFeatures = mImageFeatures[viewIdx];
                        match2D3D.points2D.push_back(newViewFeatures.points[matched2DPointInNewView]);
                        match2D3D.points3D.push_back(cloudPoint.p);
                        found2DPoint = true;
                        break;
                    }
                }

                if (found2DPoint) {
                	break;
                }
            }
        }

        matches[viewIdx] = match2D3D;
    }

    return matches;
}


void SfM::mergeNewPointCloud(const PointCloud& cloud) {
    const size_t numImages = mImages.size();
    MatchMatrix mergeMatchMatrix;
    mergeMatchMatrix.resize(numImages, vector<Matching>(numImages));

    size_t newPoints = 0;
    size_t mergedPoints = 0;

    for (const Point3DInMap& p : cloud) {
        const Point3f newPoint = p.p; //new 3D point

        bool foundAnyMatchingExistingViews = false;
        bool foundMatching3DPoint = false;
        for (Point3DInMap& existingPoint : mReconstructionCloud) {
            if (norm(existingPoint.p - newPoint) < MERGE_CLOUD_POINT_MIN_MATCH_DISTANCE) {
                //This point is very close to an existing 3D cloud point
                foundMatching3DPoint = true;

                //Look for common 2D features to confirm match
                for (const auto& newKv : p.originatingViews) {
                    //kv.first = new point's originating view
                    //kv.second = new point's view 2D feature index

                    for (const auto& existingKv : existingPoint.originatingViews) {
                        //existingKv.first = existing point's originating view
                        //existingKv.second = existing point's view 2D feature index

                        bool foundMatchingFeature = false;

						const bool newIsLeft = newKv.first < existingKv.first;
						const int leftViewIdx         = (newIsLeft) ? newKv.first  : existingKv.first;
                        const int leftViewFeatureIdx  = (newIsLeft) ? newKv.second : existingKv.second;
                        const int rightViewIdx        = (newIsLeft) ? existingKv.first  : newKv.first;
                        const int rightViewFeatureIdx = (newIsLeft) ? existingKv.second : newKv.second;


                        const Matching& matching = mFeatureMatchMatrix[leftViewIdx][rightViewIdx];
                        for (const DMatch& match : matching) {
                            if (    match.queryIdx == leftViewFeatureIdx
                                and match.trainIdx == rightViewFeatureIdx
                                and match.distance < MERGE_CLOUD_FEATURE_MIN_MATCH_DISTANCE) {

                            	mergeMatchMatrix[leftViewIdx][rightViewIdx].push_back(match);

                                //Found a 2D feature match for the two 3D points - merge
                                foundMatchingFeature = true;
                                break;
                            }
                        }

                        if (foundMatchingFeature) {
                            //Add the new originating view, and feature index
                            existingPoint.originatingViews[newKv.first] = newKv.second;
//                            cout << leftViewIdx << rightViewIdx << endl;
                            foundAnyMatchingExistingViews = true;

                        }
                    }
                }
            }
            if (foundAnyMatchingExistingViews) {
                mergedPoints++;
                break; //Stop looking for more matching cloud points
            }
        }

        if (not foundAnyMatchingExistingViews and not foundMatching3DPoint) {
            //This point did not match any existing cloud points - add it as new.
            mReconstructionCloud.push_back(p);
            newPoints++;
        }
    }

    if (mVisualDebugLevel <= LOG_DEBUG) {
        //debug: show new matching points in the cloud
        for (size_t i = 0; i < numImages - 1; i++) {
            for (size_t j = i; j < numImages; j++) {
                const Matching& matching = mergeMatchMatrix[i][j];
                if (matching.empty()) {
                    continue;
                }

                Mat outImage;
                drawMatches(mImages[i], mImageFeatures[i].keyPoints,
                            mImages[j], mImageFeatures[j].keyPoints,
                            matching, outImage);
                //write the images index...
                putText(outImage, "i" + to_string(i), Point (10,                     50), CV_FONT_NORMAL, 3.0, Colors::GREEN, 3);
                putText(outImage, "i" + to_string(j), Point (10 + outImage.cols / 2, 50), CV_FONT_NORMAL, 3.0, Colors::GREEN, 3);
                // resize(outImage, outImage, Size(), 1.25, 0.25);
                imshow("Merge Match", outImage);
                waitKey(0);
                destroyWindow("Merge Match");
            }
        }
        destroyWindow("Merge Match");
    }

    if (mConsoleDebugLevel <= LOG_DEBUG) {
        cout << " adding: " << cloud.size() << " (new: " << newPoints << ", merged: " << mergedPoints << ")" << endl;
    }
}

void SfM::saveCloudAndCamerasToPLY(const std::string& prefix) {
    if (mConsoleDebugLevel <= LOG_INFO) {
        cout << "Saving result reconstruction with prefix " << prefix << endl;
    }

    ofstream ofs(prefix + "_points.ply");

    //write PLY header
    ofs << "ply                 " << endl <<
           "format ascii 1.0    " << endl <<
           "element vertex " << mReconstructionCloud.size() << endl <<
           "property float x    " << endl <<
           "property float y    " << endl <<
           "property float z    " << endl <<
           "property uchar red  " << endl <<
           "property uchar green" << endl <<
           "property uchar blue " << endl <<
           "end_header          " << endl;

    for (const Point3DInMap& p : mReconstructionCloud) {
    	//get color from first originating view
		auto originatingView = p.originatingViews.begin();
		const int viewIdx = originatingView->first;
		Point2f p2d = mImageFeatures[viewIdx].points[originatingView->second];
		Vec3b pointColor = mImages[viewIdx].at<Vec3b>(p2d);

		//write vertex
        ofs << p.p.x              << " " <<
        	   p.p.y              << " " <<
			   p.p.z              << " " <<
			   (int)pointColor(2) << " " <<
			   (int)pointColor(1) << " " <<
			   (int)pointColor(0) << " " << endl;
    }

    ofs.close();

    ofstream ofsc(prefix + "_cameras.ply");

    //write PLY header
    ofsc << "ply                 " << endl <<
           "format ascii 1.0    " << endl <<
           "element vertex " << (mCameraPoses.size() * 4) << endl <<
           "property float x    " << endl <<
           "property float y    " << endl <<
           "property float z    " << endl <<
           "element edge " << (mCameraPoses.size() * 3) << endl <<
           "property int vertex1" << endl <<
           "property int vertex2" << endl <<
           "property uchar red  " << endl <<
           "property uchar green" << endl <<
           "property uchar blue " << endl <<
           "end_header          " << endl;

    //save cameras polygons..
    for (const auto& pose : mCameraPoses) {
        Point3d c(pose(0, 3), pose(1, 3), pose(2, 3));
        Point3d cx = c + Point3d(pose(0, 0), pose(1, 0), pose(2, 0)) * 0.2;
        Point3d cy = c + Point3d(pose(0, 1), pose(1, 1), pose(2, 1)) * 0.2;
        Point3d cz = c + Point3d(pose(0, 2), pose(1, 2), pose(2, 2)) * 0.2;

        ofsc << c.x  << " " << c.y  << " " << c.z  << endl;
        ofsc << cx.x << " " << cx.y << " " << cx.z << endl;
        ofsc << cy.x << " " << cy.y << " " << cy.z << endl;
        ofsc << cz.x << " " << cz.y << " " << cz.z << endl;
    }

//    const int camVertexStartIndex = mReconstructionCloud.size();

    for (size_t i = 0; i < mCameraPoses.size(); i++) {
        ofsc << (i * 4 + 0) << " " <<
                (i * 4 + 1) << " " <<
                "255 0 0" << endl;
        ofsc << (i * 4 + 0) << " " <<
                (i * 4 + 2) << " " <<
                "0 255 0" << endl;
        ofsc << (i * 4 + 0) << " " <<
                (i * 4 + 3) << " " <<
                "0 0 255" << endl;
    }
    ofsc.close();

    ofstream cdf(prefix + "_cam_distances_final.txt");
    dump(mCameraPoses);
    for(int i=1; i<mCameraPoses.size(); i++) {
        cdf << findCameraDistance( mCameraPoses[i-1], mCameraPoses[i] ) << endl;
    }
    cdf.close();

    ofstream cd(prefix + "_cam_distances.txt");
    double cumSum = 0.0;
    int counter = 1;
    cd << "Camera, Distance From Previous Camera, Sum of Distances" << endl;
    for (auto distance :mlinearDistances) {
        cumSum += distance;
        cd << counter << "," << distance << "," << cumSum << endl;
        counter++;
    }
    cd.close();
    
}

} /* namespace sfmtoylib */
