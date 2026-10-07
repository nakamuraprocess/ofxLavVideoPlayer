#pragma once

// ofxLavVideoPlayer
//
// A DirectShow video player for openFrameworks 0.11.2 (Windows only) that loads
// LAV Filters (LAVSplitter.ax / LAVVideo.ax / LAVAudio.ax) directly from disk,
// so no codec pack has to be installed or registered on the system.
//
// Based on ofDirectShowPlayer (DirectShowVideo by Theodore Watson, Jan 2014),
// part of openFrameworks, released under the MIT License.
//
// Usage:
//     ofVideoPlayer video;
//     video.setPlayer(std::make_shared<ofxLavVideoPlayer>());
//     video.load("movie.mkv");
//     video.play();

#include "ofConstants.h"

#ifdef TARGET_WIN32

#include "ofVideoBaseTypes.h"
#include <memory>
#include <string>

template<typename T>
class ofPixels_;

typedef ofPixels_<unsigned char> ofPixels;

namespace ofxLav {
	class DirectShowVideo;
}

class ofxLavVideoPlayer : public ofBaseVideoPlayer {
public:
	ofxLavVideoPlayer();
	ofxLavVideoPlayer(const ofxLavVideoPlayer&) = delete;
	ofxLavVideoPlayer& operator=(const ofxLavVideoPlayer&) = delete;
	ofxLavVideoPlayer(ofxLavVideoPlayer&&);
	ofxLavVideoPlayer& operator=(ofxLavVideoPlayer&&);

	// --- Codec settings (static, shared by all players) ---

	/// Folder containing LAVSplitter.ax, LAVVideo.ax, LAVAudio.ax and their FFmpeg DLLs.
	/// Absolute paths are used as-is, relative paths are resolved from bin/data/.
	/// If never set, "bin/data/codecs/" is used. Call this before load().
	static void setCodecDirectory(const std::string& directory);

	/// Returns the folder that will actually be searched for the LAV Filters.
	static std::string getCodecDirectory();

	/// When true (default), files that LAV cannot open are retried with the
	/// filters installed on the system (same behaviour as ofDirectShowPlayer).
	static void setFallbackToSystemFilters(bool fallback);
	static bool getFallbackToSystemFilters();

	// --- ofBaseVideoPlayer ---

	bool load(std::string path) override;
	void update() override;
	void close() override;

	void play() override;
	void stop() override;

	bool isFrameNew() const override;

	const ofPixels& getPixels() const override;
	ofPixels& getPixels() override;

	float getWidth() const override;
	float getHeight() const override;

	bool isPaused() const override;
	bool isLoaded() const override;
	bool isPlaying() const override;

	bool setPixelFormat(ofPixelFormat pixelFormat) override;
	ofPixelFormat getPixelFormat() const override;

	float getPosition() const override;
	float getSpeed() const override;
	float getDuration() const override;
	bool getIsMovieDone() const override;

	void setPaused(bool bPause) override;
	void setPosition(float pct) override;
	void setVolume(float volume) override; // 0..1
	void setLoopState(ofLoopType state) override;
	void setSpeed(float speed) override;
	void setFrame(int frame) override; // frame 0 = first frame...

	int getCurrentFrame() const override;
	int getTotalNumFrames() const override;
	ofLoopType getLoopState() const override;

	void firstFrame() override;
	void nextFrame() override;
	void previousFrame() override;

	// --- Extras ---

	/// True if the currently loaded movie is played through LAV Splitter Source.
	/// False if it fell back to the system filters.
	bool isUsingLav() const;

protected:
	std::shared_ptr<ofxLav::DirectShowVideo> player;
	ofPixelFormat pixelFormat;
};

#endif // TARGET_WIN32