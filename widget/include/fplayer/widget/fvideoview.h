/*************************************************
  * 描述：
  *
  * File：fvideoview.h
  * Date：2026/3/2
  * Update：
  * ************************************************/
#ifndef FPLAYER_DESKETOP_FVIDEOVIEW_H
#define FPLAYER_DESKETOP_FVIDEOVIEW_H

#include <QWidget>
#include <fplayer/widget/export.h>
#include <fplayer/api/media/ifvideoview.h>
#include <fplayer/common/fglwidget/fglwidget.h>

class QVBoxLayout;
class QVideoWidget;
class QVideoSink;

namespace fplayer
{
	class FPLAYER_WIDGET_EXPORT FVideoView : public QWidget, public IFVideoView
	{
		Q_OBJECT

	public:
		explicit FVideoView(QWidget* parent = nullptr);
		~FVideoView() override;

		// QVideoSink* videoSink() const;// 给 Qt6 backend 用
		PreviewTarget previewTarget() override;
		
		void setBackendType(MediaBackendType backendType);

	protected:
		void showEvent(QShowEvent* e) override;// 确保 winId 可用
		void resizeEvent(QResizeEvent* e) override;

	signals:
		void nativeWindowReady();// 句柄可用（可选）
		void viewResized(int w, int h, double dpr);

	private:
		QVideoWidget* m_qtVideoWidget = nullptr;
		FGLWidget* m_glWidget = nullptr;
		QVBoxLayout* m_lay = nullptr;
	};
}

#endif //FPLAYER_DESKETOP_FVIDEOVIEW_H