#ifndef FPLAYER_WIDGET_IMAGEVIEWERDIALOG_H
#define FPLAYER_WIDGET_IMAGEVIEWERDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <fplayer/widget/export.h>

class FPLAYER_WIDGET_EXPORT ImageViewerDialog : public QDialog
{
	Q_OBJECT

public:
	explicit ImageViewerDialog(QWidget* parent = nullptr);

	void setImageList(const QStringList& paths, int currentIndex = 0);
	int currentIndex() const;

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void showEvent(QShowEvent* event) override;

private slots:
	void navigateTo(int index);
	void navigatePrev();
	void navigateNext();

private:
	void updateImage();
	void updateTitle();
	void fitToWindow();
	void resetZoom();

	QScrollArea* m_scrollArea = nullptr;
	QLabel* m_imageLabel = nullptr;
	QPushButton* m_btnPrev = nullptr;
	QPushButton* m_btnNext = nullptr;
	QLabel* m_indexLabel = nullptr;
	QStringList m_paths;
	int m_currentIndex = -1;
	QPixmap m_currentPixmap;
	double m_zoomLevel = 1.0;
};

#endif // FPLAYER_WIDGET_IMAGEVIEWERDIALOG_H
