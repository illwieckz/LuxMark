 /***************************************************************************
 *   Copyright (C) 1998-2013 by authors (see AUTHORS.txt)                  *
 *                                                                         *
 *   This file is part of LuxRays.                                         *
 *                                                                         *
 *   LuxRays is free software; you can redistribute it and/or modify       *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   LuxRays is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 *                                                                         *
 *   LuxRays website: http://www.luxrender.net                             *
 ***************************************************************************/

#include <QFile>
#include <QCryptographicHash>

#include "slg/utils/convtest/convtest.h"
#include "luxmarkcfg.h"
#include "resultdialog.h"
#include "submitdialog.h"
#include "luxmarkapp.h"

static QString Path2QString(const boost::filesystem::path &fileName) {
	return QString::fromStdWString(fileName.generic_wstring());
}

ResultDialog::ResultDialog(const LuxMarkAppMode m,
		const char *scnName, const double sampSecs,
		const vector<BenchmarkDeviceDescription> &ds,
		const unsigned char *fb,
		const u_int width, const u_int height,
		QWidget *parent) : QDialog(parent),
		ui(new Ui::ResultDialog), descs(ds) {
	mode = m;
	sceneName = scnName;
	sampleSecs = sampSecs;
	frameBuffer = fb;
	frameBufferWidth = width;
	frameBufferHeight = height;
	sceneValidation = false;
	imageValidation = false;

	ui->setupUi(this);

	this->setWindowTitle("LuxMark v" LUXMARK_VERSION_MAJOR "." LUXMARK_VERSION_MINOR);
	ui->modeLabel->setText(LuxMarkAppMode2String(mode).c_str());
	ui->sceneLabel->setText(sceneName);

	deviceListModel = new DeviceListModel(descs);
	ui->deviceListView->setModel(deviceListModel);

	ui->resultLCD->display(int(sampleSecs / 1000.0));

	if ((strcmp(sceneName, SCENE_ROOM) != 0) &&
			(strcmp(sceneName, SCENE_SALA) != 0) &&
			(strcmp(sceneName, SCENE_LUXBALL_HDR) != 0))
		ui->submitButton->setEnabled(false);

	// To re-enable only with official release
	ui->submitButton->setEnabled(false);

	QObject::connect(this, SIGNAL(sceneValidationLabelChanged(const QString &, const bool)),
			this, SLOT(setSceneValidationLabel(const QString &, const bool)));
	QObject::connect(this, SIGNAL(imageValidationLabelChanged(const QString &, const bool)),
			this, SLOT(setImageValidationLabel(const QString &, const bool)));

	// Check if it ione of the official benchmarks
	if (strcmp(sceneName, SCENE_LUXBALL_HDR) == 0) {
		// Start the md5 validation thread
		md5Thread = new boost::thread(boost::bind(ResultDialog::MD5ThreadImpl, this));

		// Start the image validation thread
		imageThread = new boost::thread(boost::bind(ResultDialog::ImageThreadImpl, this));
	} else {
		ui->sceneValidation->setText("N/A");
		ui->sceneValidation->setStyleSheet("QLabel { color : green; }");
		md5Thread = NULL;

		ui->imageValidation->setText("N/A");
		ui->imageValidation->setStyleSheet("QLabel { color : green; }");
		imageThread = NULL;
	}
}

ResultDialog::~ResultDialog() {
	if (md5Thread) {
		md5Thread->join();
		delete md5Thread;
	}
	if (imageThread) {
		imageThread->join();
		delete imageThread;
	}
	
	delete ui;
	delete deviceListModel;
}

void ResultDialog::submitResult() {
	// TODO: wait for the end of all pending processes
	SubmitDialog *dialog = new SubmitDialog(sceneName, sampleSecs, descs);
	dialog->exec();
	delete dialog;
}

void ResultDialog::setSceneValidationLabel(const QString &text, const bool isOk) {
	ui->sceneValidation->setText(text);
	if (isOk)
		ui->sceneValidation->setStyleSheet("QLabel { color : green; }");
	else
		ui->sceneValidation->setStyleSheet("QLabel { color : red; }");

	sceneValidation = isOk;
}

void ResultDialog::setImageValidationLabel(const QString &text, const bool isOk) {
	ui->imageValidation->setText(text);
	if (isOk)
		ui->imageValidation->setStyleSheet("QLabel { color : green; }");
	else
		ui->imageValidation->setStyleSheet("QLabel { color : red; }");

	imageValidation = isOk;
}

void ResultDialog::AddSceneFiles(ResultDialog *resultDialog,
		vector<boost::filesystem::path> &files, const boost::filesystem::path &path) {
    for (boost::filesystem::directory_iterator it(path); it != boost::filesystem::directory_iterator(); ++it) {
		const boost::filesystem::path &fileName = it->path();

		if (boost::filesystem::is_directory(fileName)) {
			// Avoid LuxVR scene directory
			if (fileName.filename() != "luxvr-scene")
				AddSceneFiles(resultDialog, files, fileName);
		} else if (boost::filesystem::is_regular_file(fileName)) {
			// Check if it is one of the scene file extensions
			const string ext = fileName.extension().generic_string();
			if ((ext == ".lxs") || (ext == ".lxm") || (ext == ".lxo") || (ext == ".lxv") || (ext == ".ply") ||
					 (ext == ".jpg") || (ext == ".png") || (ext == ".hdr")) {
				const QString label(("Selecting file [" + fileName.filename().generic_string() + "]").c_str());
				emit resultDialog->sceneValidationLabelChanged(label, false);
				files.push_back(fileName);
			}
		}
	}
}

void ResultDialog::MD5ThreadImpl(ResultDialog *resultDialog) {
	// Begin the md5 scene validation process
	emit resultDialog->sceneValidationLabelChanged("Starting...", false);

	try {
		// Extract the scene directory name
		boost::filesystem::path scenePath = boost::filesystem::path(resultDialog->sceneName).parent_path();
		LM_LOG("MD5 validation scene path: [" << scenePath << "]");

		// Build the list of files to validate
		vector<boost::filesystem::path> files;
		AddSceneFiles(resultDialog, files, scenePath);
        sort(files.begin(), files.end());
		LM_LOG("MD5 validation selected files: [" << scenePath << "]");
		BOOST_FOREACH(boost::filesystem::path fileName, files) {
			LM_LOG("  [" << fileName << "]");
		}

		// Validate each file
		QCryptographicHash hash(QCryptographicHash::Md5);
		LM_LOG("MD5 validated files: [" << scenePath << "]");
		BOOST_FOREACH(boost::filesystem::path fileName, files) {
			LM_LOG("  [" << fileName << "]");

			const QString label(("Validating file [" + fileName.filename().generic_string() + "]").c_str());
			emit resultDialog->sceneValidationLabelChanged(label, false);

			// Read all file
			QFile file(Path2QString(fileName));
			hash.addData(file.readAll());
			file.close();
		}

		// Check the result
		const string md5 = QString(hash.result().toHex()).toStdString();
		LM_LOG("Scene files MD5: [" << md5 << "]");

		if (strcmp(resultDialog->sceneName, SCENE_LUXBALL_HDR) == 0) {
			if (md5 == "d41d8cd98f00b204e9800998ecf8427e")
				emit resultDialog->sceneValidationLabelChanged("OK", true);
			else
				emit resultDialog->sceneValidationLabelChanged("Failed", false);
		} else {
			// This should never happen, the thread should have not been started at all
			LM_LOG("Internal error in ResultDialog::MD5ThreadImpl()");
			emit resultDialog->sceneValidationLabelChanged("Failed", false);
		}
	} catch (exception &err) {
		LM_ERROR("SCENE VALIDATION ERROR: " << err.what());

		emit resultDialog->sceneValidationLabelChanged("Error", false);
	}
}

void ResultDialog::ImageThreadImpl(ResultDialog *resultDialog) {
	// Begin the image validation process
	emit resultDialog->imageValidationLabelChanged("Starting...", false);

	float *referenceImage = NULL;
	float *testImage = NULL;
	try {
		// Extract the scene directory name
		boost::filesystem::path scenePath = boost::filesystem::path(resultDialog->sceneName).parent_path();
		LM_LOG("Image validation scene path: [" << scenePath << "]");

		const u_int dataCount = resultDialog->frameBufferWidth * resultDialog->frameBufferHeight * 3;

		// Read the reference file
		if (strcmp(resultDialog->sceneName, SCENE_LUXBALL_HDR) == 0) {
			boost::filesystem::path fileName;
			// SLG benchmark
			if ((resultDialog->mode == BENCHMARK_OCL_GPU) ||
					(resultDialog->mode == BENCHMARK_OCL_CPUGPU) ||
					(resultDialog->mode == BENCHMARK_OCL_CPU) ||
					(resultDialog->mode == BENCHMARK_OCL_CUSTOM) ||
					(resultDialog->mode == BENCHMARK_HYBRID_GPU) ||
					(resultDialog->mode == BENCHMARK_HYBRID_CUSTOM) ||
					(resultDialog->mode == BENCHMARK_NATIVE_PATH)) {
				fileName = scenePath / "reference.raw";
			} else
				throw std::runtime_error("Internal error in ResultDialog::ImageThreadImpl(): unknown mode");
			LM_LOG("Image validation file name: [" << fileName << "]");

			// Read the raw data
			QFile rawFile(Path2QString(fileName));
			if (!rawFile.open(QIODevice::ReadOnly))
				throw std::runtime_error("Internal error in ResultDialog::ImageThreadImpl(): unable to open image reference file");
			const QByteArray rawData = rawFile.readAll();
			rawFile.close();

			// Check the image size
			if (rawData.size() != (int)dataCount)
				throw std::runtime_error("Internal error in ResultDialog::ImageThreadImpl(): wrong image size");

			// Create reference image
			referenceImage = new float[dataCount];
			const unsigned char *pixels = reinterpret_cast<const unsigned char *>(rawData.constData());
			for (u_int i = 0; i < dataCount; ++i)
				referenceImage[i] = pixels[i] / 255.f;
		} else
			throw std::runtime_error("Internal error in ResultDialog::ImageThreadImpl(): unknown scene");

		// Create test image
		testImage = new float[dataCount];
		for (u_int i = 0; i < dataCount; ++i)
			testImage[i] = resultDialog->frameBuffer[i] / 255.f;

		// Run the image comparison
		slg::ConvergenceTest convTest(resultDialog->frameBufferWidth, resultDialog->frameBufferHeight);

		// Reference image
		convTest.Test(referenceImage);

		// Test image
		emit resultDialog->imageValidationLabelChanged("Comparing...", false);
		const u_int diffPixelCount = convTest.Test(testImage);

		const float errorPerc =  100.f * diffPixelCount / (float)(resultDialog->frameBufferWidth * resultDialog->frameBufferHeight);
		stringstream ss;
		ss << "OK (" << diffPixelCount << " different pixels, " << fixed << setprecision(2) << errorPerc << "%)";

		emit resultDialog->imageValidationLabelChanged(ss.str().c_str(), true);
	} catch (exception &err) {
		LM_ERROR("IMAGE VALIDATION ERROR: " << err.what());

		emit resultDialog->imageValidationLabelChanged("Error", false);
	}
	
	delete[] testImage;
	delete[] referenceImage;
}
