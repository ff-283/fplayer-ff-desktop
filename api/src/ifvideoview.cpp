#include <fplayer/api/media/ifvideoview.h>

fplayer::IFVideoView::IFVideoView(MediaBackendType backendType): m_backendType(backendType)
{

}

fplayer::IFVideoView::~IFVideoView() = default;

void fplayer::IFVideoView::setBackendType(MediaBackendType backendType)
{
	m_backendType = backendType;
}