/**
 * @file video.h
 * @brief Cutscene video playback subsystem.
 */
#ifndef SOLOADER_VIDEO_H
#define SOLOADER_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Plays a video file (MP4 / M4V with H.264 video and AAC audio).
 * @param path Full filesystem path to the video file.
 * @return 1 on success, 0 on failure.
 */
int video_play(const char *path);

#ifdef __cplusplus
}
#endif

#endif // SOLOADER_VIDEO_H
