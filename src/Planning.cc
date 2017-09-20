#include "Planning.h"

#include <cmath>

#include <numeric>

#include <iostream>

void compute_std(std::vector<double> v, double & mean, double & stdev)
{
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    mean = sum / v.size();

    std::vector<double> diff(v.size());
    std::transform(v.begin(), v.end(), diff.begin(),
                std::bind2nd(std::minus<double>(), mean));
    double sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
    stdev = std::sqrt(sq_sum / v.size());
}

namespace ORB_SLAM2 {

Planning::Planning(cv::Mat goal_pose, Map* pMap){
    mpMap = pMap;
    hasRequest = false;
    std::cout << "OMPL version: " << OMPL_VERSION << std::endl;
    p_type = PLANNER_RRTSTAR;
    o_type = OBJECTIVE_PATHLENGTH;

    q_start = {0, 0.0, 0};
    q_goal = {5.03, -1.69, -1.5707};

    T_sw<<0,   -1.0000,         0,    -0.1000,
    0,         0,   -1.0000,         0,
    1.0000,         0,         0,   -0.2500,
    0,         0,        0,    1.0000;

    T_bc <<0 ,        0 ,   1.0000 ,   0.2500 ,
        -1.0000 ,        0  ,       0 ,  -0.1000,
        0 ,  -1.0000 ,        0  ,       0,
        0 ,        0  ,       0   , 1.0000;
    
    pl = new plan_slam();
  
}

void Planning::Run() {
    int counter = 1;
    while (1) {
        // Run planner when Tracking thread send request.
        if (CheckHasRequest()) {
            // Call Planner with currKF and currPose.
            cout << "in planning loop" << endl;
            planningMap.clear();
            UB.clear();
            LB.clear();
            maxDist.clear();
            minDist.clear();
            foundRatio.clear();
            keyframePose.clear();
            planningFinish = false;

            // update map here
            // 1. access to the map
            // 2. get the map points 
            // 3. compute the Upper bound, the lower bound
            {
                unique_lock<mutex> lock(mpMap->mMutexMapUpdate);
                vector<MapPoint*> vpPts = mpMap->GetAllMapPoints();
                cout << "totally " << vpPts.size() << " points." << endl;
                
                for(size_t i=0; i<vpPts.size(); i++){
                    if(vpPts[i]->isBad())
                        continue;
                    
                    planningMap.push_back(std::vector<double>{vpPts[i]->GetWorldPos().at<float>(0),
                                                             vpPts[i]->GetWorldPos().at<float>(1),
                                                             vpPts[i]->GetWorldPos().at<float>(2)});

                    if(vpPts[i]->theta_std * 2.5 < 30.0/57.3){
                        theta_interval = 30.0/57.3;
                    }else{
                        theta_interval = vpPts[i]->theta_std * 2.5;
                    }

                    UB.push_back(double(vpPts[i]->theta_mean + theta_interval));
                    LB.push_back(double(vpPts[i]->theta_mean - theta_interval));

                    maxDist.push_back(double(vpPts[i]->GetMaxDistanceInvariance()));
                    minDist.push_back(double(vpPts[i]->GetMinDistanceInvariance()));
                    foundRatio.push_back(double(vpPts[i]->GetFoundRatio()));
                }
                
                vector<KeyFrame*> vpKfs = mpMap->GetAllKeyFrames();
                for(size_t i=0; i<vpKfs.size(); i++){
                        if(vpKfs[i]->isBad()){
                            continue;
                        }
                        Eigen::Matrix4f T_sc = Converter::toMatrix4f(vpKfs[i]->GetPoseInverse());
                        T_wb = T_sw.inverse()*T_sc*T_bc.inverse();                        
                        std::vector<double> kfpose;
                        kfpose.push_back(double(T_wb(0,3)));
                        kfpose.push_back(double(T_wb(1,3)));
                        Eigen::Vector3f eulerAngleKf = T_wb.topLeftCorner<3,3>().eulerAngles(2,1,0);
                        kfpose.push_back(double(eulerAngleKf(0)));
                        keyframePose.push_back(kfpose);
                    }

            }            

            int threshold = 40;
            int threshold_explore = 0;

            cout << maxDist[0] << endl;

            pl->UpdateMap(planningMap, UB, LB, maxDist, minDist, foundRatio);
            pl->set_featureThreshold(threshold);
            pl->set_FloorMap(FloorMap);

            std::vector<double> q_curr_goal = q_goal;

            // if the goal is detected, just go for it!
            if(!currPose.empty()){
                // reset the start
                float x_curr = currPose.at<float>(0,3);
                float y_curr = currPose.at<float>(1,3);
                float curr_angle = atan2(currPose.at<float>(1,0), currPose.at<float>(0,0));
                q_start = {x_curr, y_curr, curr_angle};
            }
            
            // do actual planning
            pl->plan(q_start, q_curr_goal, 5, p_type, o_type);

            approxSolution = pl->isApproximate();

            counter ++;
            // save trajectory
            current_trajectory.clear();
            current_trajectory = pl->get_path_matrix();
            cout << "*********************** In Planning.cc *********************" << endl;
            cout << "copied planned trajectory size = " << current_trajectory.size() << endl;
            cout << "copied planned trajectory first = [ " << current_trajectory[0][0] << ", " << current_trajectory[0][1] << ", " << current_trajectory[0][2] << "] " << endl;
            cout << "copied planned trajectory last = [ " << current_trajectory[current_trajectory.size()-1][0] << ", " << current_trajectory[current_trajectory.size()-1][1] << ", " << current_trajectory[current_trajectory.size()-1][2] << "] " << endl;            
            cout << "*********************** End Planning.cc *********************" << endl;
                               

            // check the point when the visibility constrain is not satisfied
            int nxt_start = pl->AdvanceStepCamera(current_trajectory, threshold);

            if(nxt_start>-1){
                q_start = current_trajectory[nxt_start];
                
                // Set planned trajectory.
                unique_lock<mutex> trajectory_lock(mMutexTrajectory);
                planned_trajectory.insert(planned_trajectory.end(), current_trajectory.begin(), current_trajectory.begin()+nxt_start);
                trajectory_lock.unlock();
            }

            planningFinish = true;
            
            // Ack the request.
            AckRequest();
            
        }
    }
}

// This function is called from Tracking thread (System).
void Planning::SendPlanningRequest(cv::Mat pose, KeyFrame* kf) {
    unique_lock<mutex> lock(mMutexRequest);
    currPose = pose;
    currKF = kf;
    hasRequest = true;
}

std::vector<std::vector<double>> Planning::GetPlanningTrajectory() {
    unique_lock<mutex> lock(mMutexTrajectory);
    std::vector<std::vector<double>> trajectory_copy = planned_trajectory;
    return trajectory_copy;
}

void Planning::AckRequest() {
    unique_lock<mutex> lock(mMutexRequest);
    //planned_trajectory.clear(); 
    hasRequest = false;
}

void Planning::RequestFinish() {
    unique_lock<mutex> lock(mMutexFinish);
    mbFinishRequested = true;
}

float GetTranslationMatrixDistance(const cv::Mat& pose1,
                                   const cv::Mat& pose2) {
    return pow(pose1.at<float>(0, 3) - pose2.at<float>(0, 3), 2) +
           pow(pose1.at<float>(1, 3) - pose2.at<float>(1, 3), 2) +
           pow(pose1.at<float>(2, 3) - pose2.at<float>(2, 3), 2);
}

// Given a Tsc, find the set of possibly visible points from the closest key
// frame.
std::set<MapPoint*> Planning::GetVisiblePoints(cv::Mat pose) {
    unique_lock<mutex> lock(mpMap->mMutexMapUpdate);
    std::vector<KeyFrame*> key_frames = mpMap->GetAllKeyFrames();
    // Find the closest key frame.
    double min_dist =
            GetTranslationMatrixDistance(pose, key_frames.front()->GetPose());
    KeyFrame* min_kf = key_frames.front();
    for (auto* kf : key_frames) {
        float curr_dist = GetTranslationMatrixDistance(pose, kf->GetPose());
        if (min_dist > curr_dist) {
            min_dist = curr_dist;
            min_kf = kf;
        }
    }
    // Find all visible points from the key frame.
    std::set<MapPoint*> visible_mps = min_kf->GetMapPoints();
    for (auto* kf : min_kf->GetConnectedKeyFrames()) {
        std::set<MapPoint*> connected_visible_mps = kf->GetMapPoints();
        visible_mps.insert(connected_visible_mps.begin(),
                           connected_visible_mps.end());
    }
    cout << "#visible points=" << visible_mps.size() << endl;
    return visible_mps;
}

bool Planning::CheckFinish() {
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinishRequested;
}

void Planning::SetFinish() {
    unique_lock<mutex> lock(mMutexFinish);
    mbFinished = true;
    unique_lock<mutex> lock2(mMutexStop);
    mbStopped = true;
}

bool Planning::isFinished() {
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinished;
}

bool Planning::CheckHasRequest() {
    unique_lock<mutex> lock(mMutexRequest);
    return hasRequest;
}

int Planning::GetNumNewKeyFrames() {
    unique_lock<mutex> lock(mMutexKeyFrameQueue);
    return mKeyFrameQueue.size();
}

// This function is called from Tracking thread.
void Planning::InsertKeyFrame(KeyFrame* pKF) {
    unique_lock<mutex> lock(mMutexKeyFrameQueue);
    if (pKF->mnId > 0) {
        mKeyFrameQueue.push_back(pKF);
    }
}

KeyFrame* Planning::PopKeyFrameQueue(int num_pop) {
    // Pops the first num_pop items in the queue and
    // returns the last element popped.
    unique_lock<mutex> lock(mMutexKeyFrameQueue);
    KeyFrame* poppedKF;
    for (int i = 0; i < num_pop; i++) {
        poppedKF = mKeyFrameQueue.front();
        mKeyFrameQueue.pop_front();
    }
    return poppedKF;
}

void Planning::setFloorMap(vector<vector<float>> floorMap_){
    FloorMap.clear();
    for(size_t i = 0; i < floorMap_.size(); i++){
        FloorMap.push_back({double(floorMap_[i][0]), double(floorMap_[i][1]) });
    }
}

}  // namespace ORB_SLAM
