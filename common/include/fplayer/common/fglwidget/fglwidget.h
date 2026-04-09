/*************************************************
  * 描述：
  *
  * File：fglwidget.h
  * Date：2026/3/6
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_FGLWIDGET_H
#define FPLAYER_DESKETOP_FGLWIDGET_H

#include <fplayer/common/export.h>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QMutex>
#include <QByteArray>

namespace fplayer
{
	class FPLAYER_COMMON_EXPORT FGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
	{
		Q_OBJECT
	public:
		explicit FGLWidget(QWidget* parent = nullptr);
		~FGLWidget() override;

	public slots:
		void updateYUVFrame(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
		                    int width, int height, int yStride, int uStride, int vStride);

	protected:
		void initializeGL() override;
		void resizeGL(int w, int h) override;
		void paintGL() override;

	private:
		void setupShaders();
		void calculateVertices(float* vertices, int windowWidth, int windowHeight, int imageWidth, int imageHeight);
		void updateYUVTextures();

		// OpenGL 资源
		QOpenGLShaderProgram* m_program = nullptr;
		QOpenGLTexture* m_texY = nullptr;
		QOpenGLTexture* m_texU = nullptr;
		QOpenGLTexture* m_texV = nullptr;
		
		// YUV 数据
		struct YUVData {
			QByteArray yBuffer;
			QByteArray uBuffer;
			QByteArray vBuffer;
			int width = 0;
			int height = 0;
			int yStride = 0;
			int uStride = 0;
			int vStride = 0;
			bool hasData = false;
		} m_yuvData;
		
		QMutex m_mutex;
		bool m_initialized = false;
	};
}
#endif //FPLAYER_DESKETOP_FGLWIDGET_H
