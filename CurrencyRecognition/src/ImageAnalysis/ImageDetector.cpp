#include "ImageDetector.h"

// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>  <ImageDetector>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
ImageDetector::ImageDetector(Ptr<FeatureDetector> featureDetector, Ptr<DescriptorExtractor> descriptorExtractor, Ptr<DescriptorMatcher> descriptorMatcher,
	Ptr<ImagePreprocessor> imagePreprocessor, string configurationTags, string referenceImagesListPath, string testImagesListPath) :	
	_featureDetector(featureDetector), _descriptorExtractor(descriptorExtractor), _descriptorMatcher(descriptorMatcher),
	_imagePreprocessor(imagePreprocessor), _configurationTags(configurationTags), _referenceImagesListPath(referenceImagesListPath), _testImagesListPath(testImagesListPath) {
	
	setupTargetDB(referenceImagesListPath);
}


ImageDetector::~ImageDetector() {}


bool ImageDetector::setupTargetDB(const string& referenceImagesListPath) {
	ifstream imgsList(referenceImagesListPath);
	if (imgsList.is_open()) {
		string configurationLine;
		vector<string> configurations;
		while (getline(imgsList, configurationLine)) {
			configurations.push_back(configurationLine);
		}
		int numberOfFiles = configurations.size();


		cout << "    -> Initializing recognition database with " << numberOfFiles << " reference images..." << endl;
		PerformanceTimer performanceTimer;
		performanceTimer.start();

		#pragma omp parallel for schedule(dynamic)
		for (int configIndex = 0; configIndex < numberOfFiles; ++configIndex) {
			stringstream ss(configurations[configIndex]);
			string filename;
			size_t targetTag;
			string separator;
			Scalar color;			

			ss >> filename >> separator >> targetTag >> separator >> color[2] >> color[1] >> color[0];

			Mat targetImage;
			if (_imagePreprocessor->loadAndPreprocessImage(REFERENCE_IMGAGES_DIRECTORY + filename, targetImage, CV_LOAD_IMAGE_GRAYSCALE, false)) {
				string filenameWithoutExtension = ImageUtils::getFilenameWithoutExtension(filename);
				if (filenameWithoutExtension != "") {					
					stringstream maskFilename;
					maskFilename << REFERENCE_IMGAGES_DIRECTORY << filenameWithoutExtension << MASK_TOKEN << MASK_EXTENSION;

					Mat targetROIs = imread(maskFilename.str(), CV_LOAD_IMAGE_GRAYSCALE);
					if (targetROIs.data) {
						cv::threshold(targetROIs, targetROIs, 127, 255, CV_THRESH_BINARY);
						
						TargetDetector targetDetector(_featureDetector, _descriptorExtractor, _descriptorMatcher, color);						
						targetDetector.setupTargetRecognition(targetImage, targetROIs, targetTag);

						#pragma omp critical
						_targetDetectors.push_back(targetDetector);

						vector<KeyPoint>& targetKeypoints = targetDetector.getTargetKeypoints();
						stringstream imageKeypointsFilename;
						imageKeypointsFilename << REFERENCE_IMGAGES_ANALYSIS_DIRECTORY << filenameWithoutExtension << _configurationTags << IMAGE_OUTPUT_EXTENSION;
						if (targetKeypoints.empty()) {
							imwrite(imageKeypointsFilename.str(), targetImage); 
						} else {
							Mat imageKeypoints;
							cv::drawKeypoints(targetImage, targetKeypoints, imageKeypoints, TARGET_KEYPOINT_COLOR);
							imwrite(imageKeypointsFilename.str(), imageKeypoints);
						}
					}					
				}
			}									
		}

		cout << "    -> Finished initialization of targets database in " << performanceTimer.getElapsedTimeFormated() << "\n" << endl;

		return !_targetDetectors.empty();
	} else {
		return false;
	}
}


Ptr< vector< Ptr<DetectorResult> > > ImageDetector::detectTargets(Mat& image, float minimumMatchAllowed, size_t minimumNumberInliers, float minimumTargetAreaPercentage) {
	Ptr< vector< Ptr<DetectorResult> > > detectorResults(new vector< Ptr<DetectorResult> >());

	vector<KeyPoint> keypointsQueryImage;
	_featureDetector->detect(image, keypointsQueryImage);
	if (keypointsQueryImage.size() < 4) { return detectorResults; }

	Mat descriptorsQueryImage;
	_descriptorExtractor->compute(image, keypointsQueryImage, descriptorsQueryImage);

	cv::drawKeypoints(image, keypointsQueryImage, image, NONTARGET_KEYPOINT_COLOR);

	float bestMatch = 0;
	Ptr<DetectorResult> bestDetectorResult;

	int targetDetectorsSize = _targetDetectors.size();

	do {
		bestMatch = 0;

		#pragma omp parallel for schedule(dynamic)
		for (int i = 0; i < targetDetectorsSize; ++i) {
			Ptr<DetectorResult> detectorResult = _targetDetectors[i].analyzeImage(keypointsQueryImage, descriptorsQueryImage);
			float contourArea = (float)cv::contourArea(detectorResult->getTargetContour());
			float imageArea = (float)(image.cols * image.rows);
			float contourAreaPercentage = contourArea / imageArea;

			if (contourAreaPercentage > minimumTargetAreaPercentage && cv::isContourConvex(detectorResult->getTargetContour())) {
				#pragma omp critical
				{
					if (detectorResult->getBestROIMatch() > bestMatch) {
						bestMatch = detectorResult->getBestROIMatch();
						bestDetectorResult = detectorResult;
					}
				}
			}
		}

		if (bestDetectorResult.obj != NULL && bestMatch > minimumMatchAllowed) {
			if (bestDetectorResult->getInliers().size() > minimumNumberInliers) {
				detectorResults->push_back(bestDetectorResult);
			}

			// remove inliers of best match to detect more occurrences of targets
			ImageUtils::removeInliersFromKeypointsAndDescriptors(bestDetectorResult->getInliers(), keypointsQueryImage, descriptorsQueryImage);
		}		
	} while (bestMatch > minimumMatchAllowed);

	return detectorResults;
}


vector<size_t> ImageDetector::detectTargetsAndOutputResults(Mat& image, string imageFilenameWithoutExtension, bool useHighGUI) {
	Ptr< vector< Ptr<DetectorResult> > > detectorResultsOut = detectTargets(image);
	vector<size_t> results;	
	Mat imageBackup = image.clone();

	for (size_t i = 0; i < detectorResultsOut->size(); ++i) {		
		Ptr<DetectorResult> detectorResult = (*detectorResultsOut)[i];		
		results.push_back(detectorResult->getTargetValue());

		cv::drawKeypoints(image, detectorResult->getInliersKeypoints(), image, TARGET_KEYPOINT_COLOR);
		vector<Point2f> targetContour;
		targetContour = detectorResult->getTargetContour();

		stringstream ss;
		ss << detectorResult->getTargetValue();

		Mat imageMatchesSingle = imageBackup.clone();
		try {
			Rect boundingBox = cv::boundingRect(targetContour);
			ImageUtils::correctBoundingBox(boundingBox, image.cols, image.rows);
			GUIUtils::drawLabelInCenterOfROI(ss.str(), image, boundingBox);
			GUIUtils::drawLabelInCenterOfROI(ss.str(), imageMatchesSingle, boundingBox);
			ImageUtils::drawContour(image, targetContour, detectorResult->getContourColor());
			ImageUtils::drawContour(imageMatchesSingle, targetContour, detectorResult->getContourColor());
		} catch (...) {
			std::cerr << "!!! Drawing outside image !!!" << endl;
		}

		Mat matchesInliers = detectorResult->getInliersMatches(imageMatchesSingle);
		if (useHighGUI) {
			stringstream windowName;
			windowName << "Target inliers matches (window " << i << ")";
			cv::namedWindow(windowName.str(), CV_WINDOW_KEEPRATIO);
			cv::imshow(windowName.str(), matchesInliers);
			cv::waitKey(10);
		} else {
			stringstream imageOutputFilename;
			imageOutputFilename << TEST_OUTPUT_DIRECTORY << ImageUtils::getFilenameWithoutExtension(imageFilenameWithoutExtension) << FILENAME_SEPARATOR << _configurationTags << FILENAME_SEPARATOR << INLIERS_MATCHES << FILENAME_SEPARATOR << i << IMAGE_OUTPUT_EXTENSION;
			imwrite(imageOutputFilename.str(), matchesInliers);
		}		
	}	

	return results;
}


DetectorEvaluationResult ImageDetector::evaluateDetector(const string& testImgsList, bool saveResults) {
	double globalPrecision = 0;
	double globalRecall = 0;
	double globalAccuracy = 0;
	size_t numberTestImages = 0;

	stringstream resultsFilename;
	resultsFilename << TEST_OUTPUT_DIRECTORY << _configurationTags << FILENAME_SEPARATOR << RESULTS_FILE;
	ofstream resutlsFile(resultsFilename.str());

	ifstream imgsList(testImgsList);
	if (resutlsFile.is_open() && imgsList.is_open()) {
		resutlsFile << RESULTS_FILE_HEADER << "\n" << endl;

		string line;
		vector<string> imageFilenames;
		vector< vector<size_t> > expectedResults;
		while (getline(imgsList, line)) {
			stringstream lineSS(line);
			string filename;
			string separator;
			lineSS >> filename >> separator;
			imageFilenames.push_back(filename);

			vector<size_t> expectedResultFromTest;
			size_t numberExpected;
			while (lineSS >> numberExpected) {
				expectedResultFromTest.push_back(numberExpected);
			}
			expectedResults.push_back(expectedResultFromTest);
		}
		int numberOfTests = imageFilenames.size();

		cout << "    -> Evaluating detector with " << numberOfTests << " test images..." << endl;
		PerformanceTimer globalPerformanceTimer;
		globalPerformanceTimer.start();

		//#pragma omp parallel for schedule(dynamic)
		for (int i = 0; i < numberOfTests; ++i) {			
			PerformanceTimer testPerformanceTimer;
			testPerformanceTimer.start();

			string imageFilename = imageFilenames[i];
			string imageFilenameWithoutExtension = ImageUtils::getFilenameWithoutExtension(imageFilename);
			string imageFilenameWithPath = TEST_IMGAGES_DIRECTORY + imageFilenames[i];
			stringstream detectorEvaluationResultSS;
			DetectorEvaluationResult detectorEvaluationResult;
			Mat imagePreprocessed;
			cout << "\n    -> Evaluating image " << imageFilename << " (" << (i + 1) << "/" << numberOfTests << ")" << endl;
			if (_imagePreprocessor->loadAndPreprocessImage(imageFilenameWithPath, imagePreprocessed, CV_LOAD_IMAGE_GRAYSCALE, false)) {				
				vector<size_t> results = detectTargetsAndOutputResults(imagePreprocessed, imageFilenameWithoutExtension, false);
				sort(results.begin(), results.end());

				cout << "    -> Detected " << results.size() << " targets";
				size_t globalResult = 0;
				stringstream resultsSS;
				if (!results.empty()) {
					resultsSS << " (";
					for (size_t i = 0; i < results.size(); i++) {
						size_t resultValue = results[i];
						resultsSS << " " << resultValue;
						globalResult += resultValue;
					}
					resultsSS << " )";
					cout << resultsSS.str();
				}
				cout << endl;

				stringstream globalResultSS;
				globalResultSS << "Global result: " << globalResult << resultsSS.str();
				Rect globalResultBoundingBox(0, 0, imagePreprocessed.cols, imagePreprocessed.rows);
				GUIUtils::drawImageLabel(globalResultSS.str(), imagePreprocessed, globalResultBoundingBox);

				detectorEvaluationResult = DetectorEvaluationResult(results, expectedResults[i]);
				globalPrecision += detectorEvaluationResult.getPrecision();
				globalRecall += detectorEvaluationResult.getRecall();
				globalAccuracy += detectorEvaluationResult.getAccuracy();

				detectorEvaluationResultSS << PRECISION_TOKEN << ": " << detectorEvaluationResult.getPrecision() << " | " << RECALL_TOKEN << ": " << detectorEvaluationResult.getRecall() << " | " << ACCURACY_TOKEN << ": " << detectorEvaluationResult.getAccuracy();

				++numberTestImages;

				if (saveResults) {
					stringstream imageOutputFilename;
					imageOutputFilename << TEST_OUTPUT_DIRECTORY << imageFilenameWithoutExtension << FILENAME_SEPARATOR << _configurationTags << IMAGE_OUTPUT_EXTENSION;
					imwrite(imageOutputFilename.str(), imagePreprocessed);
					
					resutlsFile << imageFilename << " -> " << detectorEvaluationResultSS.str() << endl;
				}
			}
			cout << "    -> Evaluation of image " << imageFilename << " finished in " << testPerformanceTimer.getElapsedTimeFormated() << endl;
			cout << "    -> " << detectorEvaluationResultSS.str() << endl;
		}

		globalPrecision /= (double)numberTestImages;
		globalRecall /= (double)numberTestImages;
		globalAccuracy /= (double)numberTestImages;

		stringstream detectorEvaluationGloablResultSS;
		detectorEvaluationGloablResultSS << GLOBAL_PRECISION_TOKEN << ": " << globalPrecision << " | " << GLOBAL_RECALL_TOKEN << ": " << globalRecall << " | " << GLOBAL_ACCURACY_TOKEN << ": " << globalAccuracy;

		resutlsFile << "\n\n" << RESULTS_FILE_FOOTER << endl;
		resutlsFile << " ==> " << detectorEvaluationGloablResultSS.str() << endl;
		cout << "\n    -> Finished evaluation of detector in " << globalPerformanceTimer.getElapsedTimeFormated() << " || " << detectorEvaluationGloablResultSS.str() << "\n" << endl;
	}

	return DetectorEvaluationResult(globalPrecision, globalRecall, globalAccuracy);
}
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>  </ImageDetector>  <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<