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
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QMutex>
#include <QByteArray>

namespace fplayer
{
	class FPLAYER_COMMON_EXPORT FGLWidget : public QOpenGLWidget, protected QOpenGLExtraFunctions
	{
		Q_OBJECT
	public:
		explicit FGLWidget(QWidget* parent = nullptr);
		~FGLWidget() override;

	public slots:
		void updateYUVFrame(const QByteArray& yData, const QByteArray& uData, const QByteArray& vData,
		                    int width, int height, int yStride, int uStride, int vStride);
		void setColorParams(bool isBT709, bool isFullRange);

	protected:
		void initializeGL() override;
		void resizeGL(int w, int h) override;
		void paintGL() override;

	private:
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
		};

		void setupShaders();
		void calculateVertices(float* vertices, int windowWidth, int windowHeight, int imageWidth, int imageHeight);
		void updateYUVTextures(const YUVData& src);
		void uploadViaPBO(GLuint* pbo, int& pboIdx, int& pboSize, QOpenGLTexture* tex,
		                  const void* data, int width, int height);

		// OpenGL 资源
		QOpenGLShaderProgram* m_program = nullptr;
		QOpenGLTexture* m_texY = nullptr;
		QOpenGLTexture* m_texU = nullptr;
		QOpenGLTexture* m_texV = nullptr;
		GLuint m_pboY[2] = {0, 0};
		GLuint m_pboU[2] = {0, 0};
		GLuint m_pboV[2] = {0, 0};
		int m_pboIndex = 0;
		int m_pboYSize = 0;
		int m_pboUSize = 0;
		int m_pboVSize = 0;

		// Cached uniform/attribute locations
		GLint m_posLocation = -1;
		GLint m_texCoordLocation = -1;
		GLint m_texYLocation = -1;
		GLint m_texULocation = -1;
		GLint m_texVLocation = -1;
		GLint m_colorMatrixLocation = -1;
		GLint m_fullRangeLocation = -1;

		GLfloat m_vertices[16];
		YUVData m_yuvData;
		QMutex m_mutex;
		bool m_initialized = false;
		bool m_isBT709 = false;
		bool m_isFullRange = false;
		bool m_colorParamsDirty = true;
	};
}
#endif //FPLAYER_DESKETOP_FGLWIDGET_H
