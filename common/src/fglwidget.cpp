#include <fplayer/common/fglwidget/fglwidget.h>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QSurfaceFormat>
#include <QDebug>
#include <QtAlgorithms>
#include <cstring>

namespace fplayer
{
	namespace
	{
		static QByteArray repackPlaneTight(const QByteArray& src, int width, int height, int stride)
		{
			if (width <= 0 || height <= 0 || stride <= 0 || src.isEmpty())
			{
				return {};
			}
			if (stride == width)
			{
				return src;
			}
			QByteArray packed;
			packed.resize(width * height);
			const char* srcPtr = src.constData();
			char* dstPtr = packed.data();
			for (int row = 0; row < height; ++row)
			{
				std::memcpy(dstPtr + row * width, srcPtr + row * stride, static_cast<size_t>(width));
			}
			return packed;
		}

	}

	// YUV 渲染着色器 — 支持 BT.601 / BT.709 色彩矩阵 + limited/full range
	static const char* vertexShaderSource = R"(
		attribute vec2 position;
		attribute vec2 texCoord;
		varying vec2 vTexCoord;
		void main()
		{
			gl_Position = vec4(position, 0.0, 1.0);
			vTexCoord = texCoord;
		}
	)";

	static const char* fragmentShaderSource = R"(
		varying vec2 vTexCoord;
		uniform sampler2D texY;
		uniform sampler2D texU;
		uniform sampler2D texV;
		uniform int  uColorMatrix;   // 0 = BT.601, 1 = BT.709
		uniform bool uFullRange;     // true = full/jpeg range, false = limited/mpeg range

		void main()
		{
			vec3 yuv;
			yuv.x = texture2D(texY, vTexCoord).r;
			yuv.y = texture2D(texU, vTexCoord).r;
			yuv.z = texture2D(texV, vTexCoord).r;

			// Limited range → full range expansion (ITU-T H.265 / MPEG range)
			if (!uFullRange)
			{
				yuv.x = (yuv.x - 16.0/255.0) * (255.0/219.0);
				yuv.y = (yuv.y - 16.0/255.0) * (255.0/224.0);  // 128/255 centered; scale from 224
				yuv.z = (yuv.z - 16.0/255.0) * (255.0/224.0);
			}

			// Center chroma channels
			yuv.y -= 0.5;
			yuv.z -= 0.5;

			vec3 rgb;
			if (uColorMatrix == 1)
			{
				// BT.709 (HD: 720p / 1080p / 4K)
				rgb = mat3(
					1.0, 1.0, 1.0,
					0.0, -0.21482, 2.12798,
					1.28033, -0.38059, 0.0
				) * yuv;
			}
			else
			{
				// BT.601 (SD: ≤576p)
				rgb = mat3(
					1.0, 1.0, 1.0,
					0.0, -0.39465, 2.03211,
					1.13983, -0.58060, 0.0
				) * yuv;
			}

			gl_FragColor = vec4(rgb, 1.0);
		}
	)";

	FGLWidget::FGLWidget(QWidget* parent) : QOpenGLWidget(parent)
	{
	// Linux/X11 下 QOpenGLWidget 若走部分更新或背景擦除，易出现持续闪烁/抖动。
	setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
	setAttribute(Qt::WA_OpaquePaintEvent, true);
	setAttribute(Qt::WA_NoSystemBackground, true);
	setAutoFillBackground(false);

		QSurfaceFormat fmt = format();
		if (fmt.swapInterval() != 1)
		{
			fmt.setSwapInterval(1);
			setFormat(fmt);
		}
	}

	FGLWidget::~FGLWidget()
	{
		makeCurrent();
		if (m_pboY[0]) { glDeleteBuffers(2, m_pboY); }
		if (m_pboU[0]) { glDeleteBuffers(2, m_pboU); }
		if (m_pboV[0]) { glDeleteBuffers(2, m_pboV); }
		delete m_texY; m_texY = nullptr;
		delete m_texU; m_texU = nullptr;
		delete m_texV; m_texV = nullptr;
		delete m_program; m_program = nullptr;
		doneCurrent();
	}

	void FGLWidget::initializeGL()
	{
		initializeOpenGLFunctions();
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		setupShaders();
		m_initialized = true;
	}

	void FGLWidget::resizeGL(int w, int h)
	{
		glViewport(0, 0, w, h);
	}

	void FGLWidget::paintGL()
	{
		glClear(GL_COLOR_BUFFER_BIT);

		if (!m_program || m_posLocation < 0)
		{
			return;
		}

		YUVData frameSnap;
		{
			QMutexLocker locker(&m_mutex);
			if (!m_yuvData.hasData)
			{
				return;
			}
			frameSnap = m_yuvData;
		}

		m_program->bind();

		updateYUVTextures(frameSnap);

		if (!m_texY || !m_texU || !m_texV)
		{
			m_program->release();
			return;
		}

		glActiveTexture(GL_TEXTURE0);
		m_texY->bind();
		glActiveTexture(GL_TEXTURE1);
		m_texU->bind();
		glActiveTexture(GL_TEXTURE2);
		m_texV->bind();

		calculateVertices(m_vertices, width(), height(), frameSnap.width, frameSnap.height);

		glVertexAttribPointer(m_posLocation, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), m_vertices);
		glEnableVertexAttribArray(m_posLocation);
		glVertexAttribPointer(m_texCoordLocation, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), m_vertices + 2);
		glEnableVertexAttribArray(m_texCoordLocation);

		m_program->setUniformValue(m_texYLocation, 0);
		m_program->setUniformValue(m_texULocation, 1);
		m_program->setUniformValue(m_texVLocation, 2);
		m_program->setUniformValue(m_colorMatrixLocation, m_isBT709 ? 1 : 0);
		m_program->setUniformValue(m_fullRangeLocation, m_isFullRange);

		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

		glDisableVertexAttribArray(m_posLocation);
		glDisableVertexAttribArray(m_texCoordLocation);

		m_program->release();
	}

	void FGLWidget::uploadViaPBO(GLuint* pbo, int& pboIdx, int& pboSize, QOpenGLTexture* tex,
	                         const void* data, int width, int height)
	{
		const int bytes = width * height;
		if (!pbo[0])
		{
			glGenBuffers(2, pbo);
		}
		if (bytes != pboSize)
		{
			for (int i = 0; i < 2; ++i)
			{
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
				glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
			}
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			pboSize = bytes;
		}
		pboIdx = (pboIdx + 1) & 1;
		GLuint buf = pbo[pboIdx];
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf);
		void* dst = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, bytes,
		                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		if (dst)
		{
			memcpy(dst, data, bytes);
			glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
			tex->bind();
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		}
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}

	void FGLWidget::updateYUVTextures(const YUVData& src)
	{
		if (!src.hasData)
		{
			return;
		}

		const int yWidth = src.width;
		const int yHeight = src.height;
		const int uvWidth = (yWidth + 1) / 2;
		const int uvHeight = (yHeight + 1) / 2;

		auto ensureTex = [](QOpenGLTexture*& tex, int w, int h) {
			if (!tex || tex->width() != w || tex->height() != h)
			{
				delete tex;
				tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
				tex->setMinificationFilter(QOpenGLTexture::Linear);
				tex->setMagnificationFilter(QOpenGLTexture::Linear);
				tex->setWrapMode(QOpenGLTexture::ClampToEdge);
				tex->setFormat(QOpenGLTexture::R8_UNorm);
				tex->setSize(w, h);
				tex->allocateStorage();
			}
		};
		ensureTex(m_texY, yWidth, yHeight);
		ensureTex(m_texU, uvWidth, uvHeight);
		ensureTex(m_texV, uvWidth, uvHeight);

		const int yStrideEffective = src.yStride > 0 ? src.yStride : yWidth;
		const int uStrideEffective = src.uStride > 0 ? src.uStride : uvWidth;
		const int vStrideEffective = src.vStride > 0 ? src.vStride : uvWidth;
		const bool yTight = (yStrideEffective == yWidth);
		const bool uTight = (uStrideEffective == uvWidth);
		const bool vTight = (vStrideEffective == uvWidth);

		if (yTight)
		{
			uploadViaPBO(m_pboY, m_pboIndex, m_pboYSize, m_texY, src.yBuffer.constData(), yWidth, yHeight);
		}
		else
		{
			repackPlaneTightBuf(m_yTightBuf, src.yBuffer, yWidth, yHeight, yStrideEffective);
			uploadViaPBO(m_pboY, m_pboIndex, m_pboYSize, m_texY, m_yTightBuf.constData(), yWidth, yHeight);
		}

		if (uTight)
		{
			uploadViaPBO(m_pboU, m_pboIndex, m_pboUSize, m_texU, src.uBuffer.constData(), uvWidth, uvHeight);
		}
		else
		{
			repackPlaneTightBuf(m_uTightBuf, src.uBuffer, uvWidth, uvHeight, uStrideEffective);
			uploadViaPBO(m_pboU, m_pboIndex, m_pboUSize, m_texU, m_uTightBuf.constData(), uvWidth, uvHeight);
		}

		if (vTight)
		{
			uploadViaPBO(m_pboV, m_pboIndex, m_pboVSize, m_texV, src.vBuffer.constData(), uvWidth, uvHeight);
		}
		else
		{
			repackPlaneTightBuf(m_vTightBuf, src.vBuffer, uvWidth, uvHeight, vStrideEffective);
			uploadViaPBO(m_pboV, m_pboIndex, m_pboVSize, m_texV, m_vTightBuf.constData(), uvWidth, uvHeight);
		}
	}

	void FGLWidget::repackPlaneTightBuf(QByteArray& dst, const QByteArray& src, int width, int height, int stride)
	{
		const int bytes = width * height;
		if (dst.size() != bytes)
		{
			dst.resize(bytes);
		}
		const char* s = src.constData();
		char* d = dst.data();
		for (int row = 0; row < height; ++row)
		{
			std::memcpy(d + row * width, s + row * stride, static_cast<size_t>(width));
		}
	}

	void FGLWidget::setColorParams(bool isBT709, bool isFullRange)
	{
		if (m_isBT709 != isBT709 || m_isFullRange != isFullRange)
		{
			m_isBT709 = isBT709;
			m_isFullRange = isFullRange;
			m_colorParamsDirty = true;
		}
	}

	void FGLWidget::updateYUVFrame(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
	                               int width, int height, int yStride, int uStride, int vStride)
	{
		if (yData.isEmpty() || uData.isEmpty() || vData.isEmpty() || width <= 0 || height <= 0)
		{
			// qDebug() << "[FGLWidget::updateYUVFrame] Invalid parameters";
			return;
		}
		if (yStride <= 0 || uStride <= 0 || vStride <= 0)
		{
			// qDebug() << "[FGLWidget::updateYUVFrame] Invalid stride";
			return;
		}
		const int uvWidth = (width + 1) / 2;
		const int uvHeight = (height + 1) / 2;
		// 防御性校验：异常 stride（例如误传 19200）会在后续纹理上传/重打包阶段触发越界风险，直接拒收。
		const int maxReasonableYStride = qMax(width * 4, width);
		const int maxReasonableUVStride = qMax(uvWidth * 4, uvWidth);
		if (yStride > maxReasonableYStride || uStride > maxReasonableUVStride || vStride > maxReasonableUVStride)
		{
			// qDebug() << "[FGLWidget::updateYUVFrame] Reject abnormal stride"
			//          << "w/h=" << width << "x" << height
			//          << "stride=" << yStride << "/" << uStride << "/" << vStride
			//          << "max=" << maxReasonableYStride << "/" << maxReasonableUVStride;
			return;
		}
		const qsizetype yNeed = static_cast<qsizetype>(yStride) * (height - 1) + width;
		const qsizetype uNeed = static_cast<qsizetype>(uStride) * (uvHeight - 1) + uvWidth;
		const qsizetype vNeed = static_cast<qsizetype>(vStride) * (uvHeight - 1) + uvWidth;
		if (yData.size() < yNeed || uData.size() < uNeed || vData.size() < vNeed)
		{
			// qDebug() << "[FGLWidget::updateYUVFrame] Buffer smaller than stride*size";
			return;
		}

		QMutexLocker locker(&m_mutex);

		m_yuvData.width = width;
		m_yuvData.height = height;
		m_yuvData.yStride = yStride;
		m_yuvData.uStride = uStride;
		m_yuvData.vStride = vStride;
		// Qt 隐式共享：多数情况下不拷贝像素，仅占引用计数；paintGL 快照再与 GL 上传配合 UNPACK_ROW_LENGTH。
		m_yuvData.yBuffer = yData;
		m_yuvData.uBuffer = uData;
		m_yuvData.vBuffer = vData;
		m_yuvData.hasData = true;

		update();
	}

	void FGLWidget::setupShaders()
	{
		m_program = new QOpenGLShaderProgram(this);
		
		if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource))
		{
			qDebug() << "[FGLWidget] Vertex shader error:" << m_program->log();
		}
		
		if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource))
		{
			qDebug() << "[FGLWidget] Fragment shader error:" << m_program->log();
		}
		
		if (!m_program->link())
		{
			qDebug() << "[FGLWidget] Shader link error:" << m_program->log();
		}
		else
		{
			m_posLocation = m_program->attributeLocation("position");
			m_texCoordLocation = m_program->attributeLocation("texCoord");
			m_texYLocation = m_program->uniformLocation("texY");
			m_texULocation = m_program->uniformLocation("texU");
			m_texVLocation = m_program->uniformLocation("texV");
			m_colorMatrixLocation = m_program->uniformLocation("uColorMatrix");
			m_fullRangeLocation = m_program->uniformLocation("uFullRange");
			qDebug() << "[FGLWidget] YUV shaders compiled successfully";
		}
	}

	void FGLWidget::calculateVertices(float* vertices, int windowWidth, int windowHeight, int imageWidth, int imageHeight)
	{
		if (windowWidth <= 0 || windowHeight <= 0 || imageWidth <= 0 || imageHeight <= 0)
		{
			return;
		}

		// 计算窗口和图像的宽高比
		float windowAspect = static_cast<float>(windowWidth) / windowHeight;
		float imageAspect = static_cast<float>(imageWidth) / imageHeight;

		// contain 等比缩放：缩放系数取两轴最小值
		float scaleX = 1.0f;
		float scaleY = 1.0f;

		float left, right, top, bottom;

		if (windowAspect > imageAspect)
		{
			// 窗口更宽：高度贴满，左右留黑边
			scaleX = imageAspect / windowAspect;
		}
		else
		{
			// 窗口更高：宽度贴满，上下留黑边
			scaleY = windowAspect / imageAspect;
		}

		left = -scaleX;
		right = scaleX;
		top = scaleY;
		bottom = -scaleY;

		// 顶点顺序：左下、右下、右上、左上
		// 纹理坐标采用正常方向，避免上下翻转
		vertices[0] = left;
		vertices[1] = bottom;
		vertices[2] = 0.0f;
		vertices[3] = 1.0f;

		vertices[4] = right;
		vertices[5] = bottom;
		vertices[6] = 1.0f;
		vertices[7] = 1.0f;

		vertices[8] = right;
		vertices[9] = top;
		vertices[10] = 1.0f;
		vertices[11] = 0.0f;

		vertices[12] = left;
		vertices[13] = top;
		vertices[14] = 0.0f;
		vertices[15] = 0.0f;
	}
}
