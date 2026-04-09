#include <fplayer/common/fglwidget/fglwidget.h>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QDebug>
#include <QtAlgorithms>
#include <cstring>

namespace fplayer
{
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
			qDebug() << "[FGLWidget::paintGL] No shader program";
			return;
		}

		if (!m_yuvData.hasData)
		{
			qDebug() << "[FGLWidget::paintGL] No YUV data";
			return;
		}

		m_program->bind();

		// 锁保护 CPU 侧帧缓存，避免 updateYUVFrame 与 paintGL 并发读写。
		QMutexLocker locker(&m_mutex);

		updateYUVTextures();

		if (!m_texY || !m_texU || !m_texV)
		{
			locker.unlock();
			m_program->release();
			return;
		}

		// 绑定纹理单元
		glActiveTexture(GL_TEXTURE0);
		m_texY->bind();
		glActiveTexture(GL_TEXTURE1);
		m_texU->bind();
		glActiveTexture(GL_TEXTURE2);
		m_texV->bind();

		locker.unlock();

		// 每帧按当前窗口/图像比例计算顶点，实现 contain 等比显示（黑边而非拉伸）。
		GLfloat vertices[16];
		calculateVertices(vertices, width(), height(), m_yuvData.width, m_yuvData.height);

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

	void FGLWidget::updateYUVTextures()
	{
		if (!m_yuvData.hasData)
		{
			return;
		}

		// YUV420P: Y 平面全分辨率，U/V 平面各为 1/2 宽高。
		int yWidth = m_yuvData.width;
		int yHeight = m_yuvData.height;
		int uvWidth = yWidth / 2;
		int uvHeight = yHeight / 2;

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

		// 使用 GL_RED + R8_UNorm，兼容性优于旧的 GL_LUMINANCE 路径。
		// 上传 Y 数据
		m_texY->bind();
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, yWidth, yHeight, GL_RED, GL_UNSIGNED_BYTE, m_yuvData.yBuffer.constData());

		// 上传 U 数据
		m_texU->bind();
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, m_yuvData.uBuffer.constData());

		// 上传 V 数据
		m_texV->bind();
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, m_yuvData.vBuffer.constData());
	}

	void FGLWidget::updateYUVFrame(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
	                               int width, int height, int yStride, int uStride, int vStride)
	{
		if (yData.isEmpty() || uData.isEmpty() || vData.isEmpty() || width <= 0 || height <= 0)
		{
			qDebug() << "[FGLWidget::updateYUVFrame] Invalid parameters";
			return;
		}

		QMutexLocker locker(&m_mutex);
		const bool formatChanged = (m_yuvData.width != width) ||
		                           (m_yuvData.height != height) ||
		                           (m_yuvData.yStride != width) ||
		                           (m_yuvData.uStride != (width / 2)) ||
		                           (m_yuvData.vStride != (width / 2));
		const bool firstFrame = !m_yuvData.hasData;

		// 复制 YUV 数据
		m_yuvData.width = width;
		m_yuvData.height = height;
		const int uvWidth = width / 2;
		const int uvHeight = height / 2;

		// 重打包为紧密内存（stride == width / uvWidth）：
		// 这样渲染阶段不依赖 GL_UNPACK_ROW_LENGTH，兼容更多驱动/上下文。
		m_yuvData.yStride = width;
		m_yuvData.uStride = uvWidth;
		m_yuvData.vStride = uvWidth;

		m_yuvData.yBuffer.resize(width * height);
		m_yuvData.uBuffer.resize(uvWidth * uvHeight);
		m_yuvData.vBuffer.resize(uvWidth * uvHeight);

		for (int row = 0; row < height; ++row)
		{
			memcpy(m_yuvData.yBuffer.data() + row * width, yData.constData() + row * yStride, width);
		}
		for (int row = 0; row < uvHeight; ++row)
		{
			memcpy(m_yuvData.uBuffer.data() + row * uvWidth, uData.constData() + row * uStride, uvWidth);
			memcpy(m_yuvData.vBuffer.data() + row * uvWidth, vData.constData() + row * vStride, uvWidth);
		}
		
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
