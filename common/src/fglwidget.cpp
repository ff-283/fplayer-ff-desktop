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

		static bool canUseUnpackRowLength()
		{
#ifdef Q_OS_WIN
			// 经验上 Windows + 部分驱动对 GL_UNPACK_ROW_LENGTH 的实现不稳定，可能导致行错位/扭曲；
			// 统一走逐行重打包路径，优先保证屏幕捕获预览稳定性。
			return false;
#else
			const QOpenGLContext* ctx = QOpenGLContext::currentContext();
			if (!ctx)
			{
				return false;
			}
			if (!ctx->isOpenGLES())
			{
				return true;
			}
			const QSurfaceFormat fmt = ctx->format();
			if (fmt.majorVersion() >= 3)
			{
				return true;
			}
			return ctx->hasExtension(QByteArrayLiteral("GL_EXT_unpack_subimage"));
#endif
		}
	}

	// YUV 渲染着色器
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
		
		void main()
		{
			vec3 yuv;
			yuv.x = texture2D(texY, vTexCoord).r;
			yuv.y = texture2D(texU, vTexCoord).r - 0.5;
			yuv.z = texture2D(texV, vTexCoord).r - 0.5;
			
			vec3 rgb = mat3(
				1.0, 1.0, 1.0,
				0.0, -0.39465, 2.03211,
				1.13983, -0.58060, 0.0
			) * yuv;
			
			gl_FragColor = vec4(rgb, 1.0);
		}
	)";

	FGLWidget::FGLWidget(QWidget* parent) : QOpenGLWidget(parent), QOpenGLFunctions()
	{
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
		
		if (m_texY)
		{
			delete m_texY;
			m_texY = nullptr;
		}
		if (m_texU)
		{
			delete m_texU;
			m_texU = nullptr;
		}
		if (m_texV)
		{
			delete m_texV;
			m_texV = nullptr;
		}
		if (m_program)
		{
			delete m_program;
			m_program = nullptr;
		}
		
		doneCurrent();
	}

	void FGLWidget::initializeGL()
	{
		initializeOpenGLFunctions();
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
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

		if (!m_program)
		{
			static int noProgramLogCounter = 0;
			if (++noProgramLogCounter % 300 == 0)
			{
				qDebug() << "[FGLWidget::paintGL] No shader program";
			}
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

		// 每帧按当前窗口/图像比例计算顶点，实现 contain 等比显示（黑边而非拉伸）。
		GLfloat vertices[16];
		calculateVertices(vertices, width(), height(), frameSnap.width, frameSnap.height);

		GLint positionLocation = m_program->attributeLocation("position");
		GLint texCoordLocation = m_program->attributeLocation("texCoord");
		GLint texYLocation = m_program->uniformLocation("texY");
		GLint texULocation = m_program->uniformLocation("texU");
		GLint texVLocation = m_program->uniformLocation("texV");

		glVertexAttribPointer(positionLocation, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
		glEnableVertexAttribArray(positionLocation);
		glVertexAttribPointer(texCoordLocation, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
		glEnableVertexAttribArray(texCoordLocation);

		m_program->setUniformValue(texYLocation, 0);
		m_program->setUniformValue(texULocation, 1);
		m_program->setUniformValue(texVLocation, 2);

		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

		glDisableVertexAttribArray(positionLocation);
		glDisableVertexAttribArray(texCoordLocation);

		m_program->release();
	}

	void FGLWidget::updateYUVTextures(const YUVData& src)
	{
		if (!src.hasData)
		{
			return;
		}

		// YUV420P: Y 平面全分辨率，U/V 平面各为 1/2 宽高。
		int yWidth = src.width;
		int yHeight = src.height;
		int uvWidth = (yWidth + 1) / 2;
		int uvHeight = (yHeight + 1) / 2;

		// 创建或更新 Y 纹理
		if (!m_texY || m_texY->width() != yWidth || m_texY->height() != yHeight)
		{
			delete m_texY;
			m_texY = new QOpenGLTexture(QOpenGLTexture::Target2D);
			m_texY->setMinificationFilter(QOpenGLTexture::Linear);
			m_texY->setMagnificationFilter(QOpenGLTexture::Linear);
			m_texY->setWrapMode(QOpenGLTexture::ClampToEdge);
			m_texY->setFormat(QOpenGLTexture::R8_UNorm);
			m_texY->setSize(yWidth, yHeight);
			m_texY->allocateStorage();
		}

		// 创建或更新 U 纹理
		if (!m_texU || m_texU->width() != uvWidth || m_texU->height() != uvHeight)
		{
			delete m_texU;
			m_texU = new QOpenGLTexture(QOpenGLTexture::Target2D);
			m_texU->setMinificationFilter(QOpenGLTexture::Linear);
			m_texU->setMagnificationFilter(QOpenGLTexture::Linear);
			m_texU->setWrapMode(QOpenGLTexture::ClampToEdge);
			m_texU->setFormat(QOpenGLTexture::R8_UNorm);
			m_texU->setSize(uvWidth, uvHeight);
			m_texU->allocateStorage();
		}

		// 创建或更新 V 纹理
		if (!m_texV || m_texV->width() != uvWidth || m_texV->height() != uvHeight)
		{
			delete m_texV;
			m_texV = new QOpenGLTexture(QOpenGLTexture::Target2D);
			m_texV->setMinificationFilter(QOpenGLTexture::Linear);
			m_texV->setMagnificationFilter(QOpenGLTexture::Linear);
			m_texV->setWrapMode(QOpenGLTexture::ClampToEdge);
			m_texV->setFormat(QOpenGLTexture::R8_UNorm);
			m_texV->setSize(uvWidth, uvHeight);
			m_texV->allocateStorage();
		}

		// 单通道纹理上传时，使用 1 字节对齐避免行对齐导致的错位。
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		const bool useRowLength = canUseUnpackRowLength();

		const QByteArray yTight = useRowLength ? QByteArray() : repackPlaneTight(src.yBuffer, yWidth, yHeight, src.yStride > 0 ? src.yStride : yWidth);
		const QByteArray uTight = useRowLength ? QByteArray() : repackPlaneTight(src.uBuffer, uvWidth, uvHeight, src.uStride > 0 ? src.uStride : uvWidth);
		const QByteArray vTight = useRowLength ? QByteArray() : repackPlaneTight(src.vBuffer, uvWidth, uvHeight, src.vStride > 0 ? src.vStride : uvWidth);

		m_texY->bind();
		if (useRowLength)
		{
			glPixelStorei(GL_UNPACK_ROW_LENGTH, src.yStride > 0 ? src.yStride : yWidth);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, yWidth, yHeight, GL_RED, GL_UNSIGNED_BYTE, src.yBuffer.constData());
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		}
		else
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, yWidth, yHeight, GL_RED, GL_UNSIGNED_BYTE, yTight.constData());
		}

		m_texU->bind();
		if (useRowLength)
		{
			glPixelStorei(GL_UNPACK_ROW_LENGTH, src.uStride > 0 ? src.uStride : uvWidth);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, src.uBuffer.constData());
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		}
		else
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, uTight.constData());
		}

		m_texV->bind();
		if (useRowLength)
		{
			glPixelStorei(GL_UNPACK_ROW_LENGTH, src.vStride > 0 ? src.vStride : uvWidth);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, src.vBuffer.constData());
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		}
		else
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, vTight.constData());
		}
	}

	void FGLWidget::updateYUVFrame(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
	                               int width, int height, int yStride, int uStride, int vStride)
	{
		if (yData.isEmpty() || uData.isEmpty() || vData.isEmpty() || width <= 0 || height <= 0)
		{
			qDebug() << "[FGLWidget::updateYUVFrame] Invalid parameters";
			return;
		}
		if (yStride <= 0 || uStride <= 0 || vStride <= 0)
		{
			qDebug() << "[FGLWidget::updateYUVFrame] Invalid stride";
			return;
		}
		const int uvWidth = (width + 1) / 2;
		const int uvHeight = (height + 1) / 2;
		const qsizetype yNeed = static_cast<qsizetype>(yStride) * (height - 1) + width;
		const qsizetype uNeed = static_cast<qsizetype>(uStride) * (uvHeight - 1) + uvWidth;
		const qsizetype vNeed = static_cast<qsizetype>(vStride) * (uvHeight - 1) + uvWidth;
		if (yData.size() < yNeed || uData.size() < uNeed || vData.size() < vNeed)
		{
			qDebug() << "[FGLWidget::updateYUVFrame] Buffer smaller than stride*size";
			return;
		}

		QMutexLocker locker(&m_mutex);
		const bool formatChanged = (m_yuvData.width != width) ||
		                           (m_yuvData.height != height) ||
		                           (m_yuvData.yStride != yStride) ||
		                           (m_yuvData.uStride != uStride) ||
		                           (m_yuvData.vStride != vStride);
		const bool firstFrame = !m_yuvData.hasData;

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
		// 仅在首帧、格式变化、低频心跳时打印，避免逐帧日志拖慢 UI 线程。
		static int heartbeatCounter = 0;
		if (firstFrame || formatChanged || (++heartbeatCounter % 300 == 0))
		{
			qDebug() << "[FGLWidget::updateYUVFrame] Frame:" << width << "x" << height
			         << "Y stride:" << yStride << "U stride:" << uStride << "V stride:" << vStride;
		}

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
