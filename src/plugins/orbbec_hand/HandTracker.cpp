// Undeprecate CRT functions
#ifndef _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

#include "HandTracker.h"
#include "Segmentation.h"
#include <SenseKitUL/streams/hand_types.h>
#include <SenseKitUL/skul_ctypes.h>
#include <SenseKit/Plugins/PluginKit.h>

namespace sensekit { namespace plugins { namespace hand {

        using namespace std;

        const int PROCESSING_SIZE_WIDTH = 80;
        const int PROCESSING_SIZE_HEIGHT = 60;

        HandTracker::HandTracker(PluginServiceProxy& pluginService,
                                 Sensor& streamset,
                                 StreamDescription& depthDescription) :
            m_pluginService(pluginService),
            m_depthUtility(PROCESSING_SIZE_WIDTH, PROCESSING_SIZE_HEIGHT),
            m_reader(streamset.create_reader()),
            m_depthStream(nullptr)
        {
            create_streams(pluginService, streamset);

            subscribe_to_depth_stream(streamset, depthDescription);
        }

        HandTracker::~HandTracker()
        {
        }

        void HandTracker::create_streams(PluginServiceProxy& pluginService, Sensor streamset)
        {
            m_handStream = make_unique<HandStream>(pluginService, streamset, SENSEKIT_HANDS_MAX_HAND_COUNT);

            const int bytesPerPixel = 3;

            m_debugImageStream = make_unique<DebugHandStream>(pluginService,
                                                              streamset,
                                                              PROCESSING_SIZE_WIDTH,
                                                              PROCESSING_SIZE_HEIGHT,
                                                              bytesPerPixel);
        }

        void HandTracker::subscribe_to_depth_stream(Sensor& streamset, StreamDescription& depthDescription)
        {
            m_depthStream = m_reader.stream<DepthStream>(depthDescription.get_subtype());
            m_depthStream.start();

            m_mapper = std::make_unique<ScalingCoordinateMapper>(m_depthStream.coordinateMapper(), 1.0f);
            m_pointProcessor = std::make_unique<PointProcessor>(m_depthStream.coordinateMapper());

            m_reader.addListener(*this);
        }

        void HandTracker::on_frame_ready(StreamReader& reader, Frame& frame)
        {
            if (m_handStream->has_connections() ||
                m_debugImageStream->has_connections())
            {
                DepthFrame depthFrame = frame.get<DepthFrame>();

                update_tracking(depthFrame);
            }
        }

        void HandTracker::reset()
        {
            m_depthUtility.reset();
            m_pointProcessor.reset();
        }

        void HandTracker::update_tracking(DepthFrame& depthFrame)
        {
            m_depthUtility.processDepthToForeground(depthFrame, m_matDepth, m_matDepthFullSize, m_matForeground);

            track_points(m_matDepth, m_matDepthFullSize, m_matForeground);

            //use same frameIndex as source depth frame
            sensekit_frame_index_t frameIndex = depthFrame.frameIndex();

            if (m_handStream->has_connections())
            {
                generate_hand_frame(frameIndex);
            }

            if (m_debugImageStream->has_connections())
            {
                generate_hand_debug_image_frame(frameIndex);
            }
        }

        void HandTracker::track_points(cv::Mat& matDepth, 
                                       cv::Mat& matDepthFullSize, 
                                       cv::Mat& matForeground)
        {
            m_layerSegmentation = cv::Mat::zeros(matDepth.size(), CV_8UC1);
            m_layerScore = cv::Mat::zeros(matDepth.size(), CV_32FC1);
            m_layerEdgeDistance = cv::Mat::zeros(matDepth.size(), CV_32FC1);
            m_debugUpdateSegmentation = cv::Mat::zeros(matDepth.size(), CV_8UC1);
            m_debugCreateSegmentation = cv::Mat::zeros(matDepth.size(), CV_8UC1);
            m_updateForegroundSearched = cv::Mat::zeros(matDepth.size(), CV_8UC1);
            m_createForegroundSearched = cv::Mat::zeros(matDepth.size(), CV_8UC1);
            m_matDepthWindow = cv::Mat::zeros(m_matDepth.size(), CV_32FC1);

            bool debugLayersEnabled = m_debugImageStream->has_connections();
            
            TrackingMatrices updateMatrices(matDepthFullSize,
                                            matDepth,
                                            m_matArea,
                                            m_matAreaSqrt,
                                            m_matBasicScore,
                                            matForeground,
                                            m_updateForegroundSearched,
                                            m_layerSegmentation,
                                            m_layerScore,
                                            m_layerEdgeDistance,
                                            m_debugUpdateSegmentation,
                                            debugLayersEnabled);

            m_pointProcessor->initialize_common_calculations(updateMatrices);

            //Update existing points first so that if we lose a point, we might recover it in the "add new" stage below
            //without having at least one frame of a lost point.

            m_pointProcessor->updateTrackedPoints(updateMatrices);

            m_pointProcessor->removeDuplicatePoints();

            TrackingMatrices createMatrices(matDepthFullSize,
                                            matDepth,
                                            m_matArea,
                                            m_matAreaSqrt,
                                            m_matBasicScore,
                                            matForeground,
                                            m_createForegroundSearched,
                                            m_layerSegmentation,
                                            m_layerScore,
                                            m_layerEdgeDistance,
                                            m_debugCreateSegmentation,
                                            debugLayersEnabled);

            //add new points (unless already tracking)
            if (!m_debugImageStream->use_mouse_probe())
            {
                cv::Point seedPosition;
                cv::Point nextSearchStart(0, 0);
                while (segmentation::find_next_foreground_pixel(matForeground, m_createForegroundSearched, seedPosition, nextSearchStart))
                {
                    m_pointProcessor->updateTrackedPointOrCreateNewPointFromSeedPosition(createMatrices, seedPosition);
                }
            }
            else
            {
                auto normPosition = m_debugImageStream->mouse_norm_position();
                int x = MAX(0, MIN(PROCESSING_SIZE_WIDTH, normPosition.x * PROCESSING_SIZE_WIDTH));
                int y = MAX(0, MIN(PROCESSING_SIZE_HEIGHT, normPosition.y * PROCESSING_SIZE_HEIGHT));
                cv::Point seedPosition(x, y);
                m_pointProcessor->updateTrackedPointOrCreateNewPointFromSeedPosition(createMatrices, seedPosition);

                float area = m_pointProcessor->get_point_area(createMatrices, seedPosition);
                float depth = matDepth.at<float>(seedPosition);
                float score = m_layerScore.at<float>(seedPosition);
                float edgeDist = m_layerEdgeDistance.at<float>(seedPosition);
                printf("probe depth: %f area: %f score: %f edgeDist %f\n", depth, area, score, edgeDist);
            }

            //remove old points
            m_pointProcessor->removeOldOrDeadPoints();

            TrackingMatrices refinementMatrices(matDepthFullSize,
                                                m_matDepthWindow, 
                                                m_matArea, 
                                                m_matAreaSqrt,
                                                m_matBasicScore, 
                                                matForeground, 
                                                m_createForegroundSearched,
                                                m_layerSegmentation,
                                                m_layerScore,
                                                m_layerEdgeDistance,
                                                m_debugUpdateSegmentation, 
                                                false);

            m_pointProcessor->update_full_resolution_points(refinementMatrices);
        }

        void HandTracker::generate_hand_frame(sensekit_frame_index_t frameIndex)
        {
            sensekit_handframe_wrapper_t* handFrame = m_handStream->begin_write(frameIndex);

            if (handFrame != nullptr)
            {
                handFrame->frame.handpoints = reinterpret_cast<sensekit_handpoint_t*>(&(handFrame->frame_data));
                handFrame->frame.handCount = SENSEKIT_HANDS_MAX_HAND_COUNT;

                update_hand_frame(m_pointProcessor->get_trackedPoints(), handFrame->frame);

                m_handStream->end_write();
            }
        }

        void HandTracker::generate_hand_debug_image_frame(sensekit_frame_index_t frameIndex)
        {
            sensekit_imageframe_wrapper_t* debugImageFrame = m_debugImageStream->begin_write(frameIndex);

            if (debugImageFrame != nullptr)
            {
                debugImageFrame->frame.data = reinterpret_cast<uint8_t *>(&(debugImageFrame->frame_data));

                sensekit_image_metadata_t metadata;

                metadata.width = PROCESSING_SIZE_WIDTH;
                metadata.height = PROCESSING_SIZE_HEIGHT;
                metadata.bytesPerPixel = 3;

                debugImageFrame->frame.metadata = metadata;
                update_debug_image_frame(debugImageFrame->frame);

                m_debugImageStream->end_write();
            }
        }

        void HandTracker::update_hand_frame(vector<TrackedPoint>& internalTrackedPoints, _sensekit_handframe& frame)
        {
            int handIndex = 0;
            int maxHandCount = frame.handCount;

            bool includeCandidates = m_handStream->include_candidate_points();

            for (auto it = internalTrackedPoints.begin(); it != internalTrackedPoints.end(); ++it)
            {
                TrackedPoint internalPoint = *it;

                TrackingStatus status = internalPoint.trackingStatus;
                TrackedPointType pointType = internalPoint.pointType;

                bool includeByStatus = status == Tracking ||
                                       status == Lost;
                bool includeByType = pointType == ActivePoint ||
                                     (pointType == CandidatePoint && includeCandidates);
                if (includeByStatus && includeByType && handIndex < maxHandCount)
                {
                    sensekit_handpoint_t& point = frame.handpoints[handIndex];
                    ++handIndex;

                    point.trackingId = internalPoint.trackingId;

                    point.depthPosition.x = internalPoint.fullSizePosition.x;
                    point.depthPosition.y = internalPoint.fullSizePosition.y;

                    copy_position(internalPoint.fullSizeWorldPosition, point.worldPosition);
                    copy_position(internalPoint.fullSizeWorldDeltaPosition, point.worldDeltaPosition);

                    point.status = convert_hand_status(status, pointType);
                }
            }
            for (int i = handIndex; i < maxHandCount; ++i)
            {
                sensekit_handpoint_t& point = frame.handpoints[i];
                reset_hand_point(point);
            }
        }

        void HandTracker::copy_position(cv::Point3f& source, sensekit_vector3f_t& target)
        {
            target.x = source.x;
            target.y = source.y;
            target.z = source.z;
        }

        sensekit_handstatus_t HandTracker::convert_hand_status(TrackingStatus status, TrackedPointType type)
        {
            if (type == TrackedPointType::CandidatePoint)
            {
                return HAND_STATUS_CANDIDATE;
            }
            switch (status)
            {
            case Tracking:
                return HAND_STATUS_TRACKING;
                break;
            case Lost:
                return HAND_STATUS_LOST;
                break;
            case Dead:
            case NotTracking:
            default:
                return HAND_STATUS_NOTTRACKING;
                break;
            }
        }

        void HandTracker::reset_hand_point(sensekit_handpoint_t& point)
        {
            point.trackingId = -1;
            point.status = HAND_STATUS_NOTTRACKING;
            point.depthPosition = sensekit_vector2i_t();
            point.worldPosition = sensekit_vector3f_t();
            point.worldDeltaPosition = sensekit_vector3f_t();
        }

        void HandTracker::update_debug_image_frame(_sensekit_imageframe& colorFrame)
        {
            float m_maxVelocity = 0.1;

            RGBPixel foregroundColor(0, 0, 255);
            RGBPixel searchedColor(128, 255, 0);

            DebugHandViewType view = m_debugImageStream->view_type();

            switch (view)
            {
            case DEBUG_HAND_VIEW_DEPTH:
                m_debugVisualizer.showDepthMat(m_matDepth,
                                               colorFrame);
                break;
            case DEBUG_HAND_VIEW_VELOCITY:
                m_debugVisualizer.showVelocityMat(m_depthUtility.matDepthVel(),
                                                  m_maxVelocity,
                                                  colorFrame);
                break;
            case DEBUG_HAND_VIEW_FILTEREDVELOCITY:
                m_debugVisualizer.showVelocityMat(m_depthUtility.matDepthVelErode(),
                                                  m_maxVelocity,
                                                  colorFrame);
                break;
            case DEBUG_HAND_VIEW_UPDATE_SEGMENTATION:
                m_debugVisualizer.showNormArray<char>(m_debugUpdateSegmentation,
                                                      m_debugUpdateSegmentation,
                                                      colorFrame);
                break;
            case DEBUG_HAND_VIEW_CREATE_SEGMENTATION:
                m_debugVisualizer.showNormArray<char>(m_debugCreateSegmentation,
                                                      m_debugCreateSegmentation,
                                                      colorFrame);
                break;
            case DEBUG_HAND_VIEW_UPDATE_SEARCHED:
            case DEBUG_HAND_VIEW_CREATE_SEARCHED:
                m_debugVisualizer.showDepthMat(m_matDepth,
                                               colorFrame);
                break;
            case DEBUG_HAND_VIEW_SCORE:
                m_debugVisualizer.showNormArray<float>(m_matBasicScore,
                                                       m_layerSegmentation, //m_debugUpdateSegmentation,
                                                       colorFrame);
                break;
            case DEBUG_HAND_VIEW_HANDWINDOW:
                m_debugVisualizer.showDepthMat(m_matDepthWindow,
                                               colorFrame);
                break;
            case DEBUG_HAND_VIEW_EDGEDISTANCE:
                m_debugVisualizer.showNormArray<float>(m_layerScore,
                                                       m_layerSegmentation, //m_debugUpdateSegmentation,
                                                       colorFrame);
                break;
            }

            if (view != DEBUG_HAND_VIEW_HANDWINDOW)
            {
                if (view == DEBUG_HAND_VIEW_CREATE_SEARCHED)
                {
                    m_debugVisualizer.overlayMask(m_createForegroundSearched, colorFrame, searchedColor);
                }
                else if (view == DEBUG_HAND_VIEW_UPDATE_SEARCHED)
                {
                    m_debugVisualizer.overlayMask(m_updateForegroundSearched, colorFrame, searchedColor);
                }

                m_debugVisualizer.overlayMask(m_matForeground, colorFrame, foregroundColor);
            }
        }
}}}