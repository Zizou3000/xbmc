// Modified for subtitle fix
/* 
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogSubtitles.h"
#include "FileItem.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "addons/AddonManager.h"
#include "addons/addoninfo/AddonInfo.h"
#include "addons/addoninfo/AddonType.h"
#include "addons/gui/GUIDialogAddonSettings.h"
#include "application/Application.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/IPlayer.h"
#include "dialogs/GUIDialogContextMenu.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/AddonsDirectory.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "filesystem/StackDirectory.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIKeyboardFactory.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "input/actions/ActionIDs.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "utils/JobManager.h"
#include "utils/LangCodeExpander.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "video/VideoDatabase.h"
#include <mutex>

using namespace ADDON;
using namespace XFILE;

namespace
{
constexpr int CONTROL_NAMELABEL = 100;
constexpr int CONTROL_NAMELOGO = 110;
constexpr int CONTROL_SUBLIST = 120;
constexpr int CONTROL_SUBSEXIST = 130;
constexpr int CONTROL_SUBSTATUS = 140;
constexpr int CONTROL_SERVICELIST = 150;
constexpr int CONTROL_MANUALSEARCH = 160;

enum class SUBTITLE_SERVICE_CONTEXT_BUTTONS
{
  ADDON_SETTINGS,
  ADDON_DISABLE
};
} // namespace

class CSubtitlesJob: public CJob
{
public:
  CSubtitlesJob(const CURL &url, const std::string &language) : m_url(url), m_language(language)
  {
    m_items = new CFileItemList;
  }
  ~CSubtitlesJob() override
  {
    delete m_items;
  }
  bool DoWork() override
  {
    CDirectory::GetDirectory(m_url.Get(), *m_items, "", DIR_FLAG_DEFAULTS);
    return true;
  }
  bool operator==(const CJob *job) const override
  {
    if (strcmp(job->GetType(),GetType()) == 0)
    {
      const CSubtitlesJob* rjob = dynamic_cast<const CSubtitlesJob*>(job);
      if (rjob)
      {
        return m_url.Get() == rjob->m_url.Get() &&
               m_language == rjob->m_language;
      }
    }
    return false;
  }
  const CFileItemList *GetItems() const { return m_items; }
  const CURL &GetURL() const { return m_url; }
  const std::string &GetLanguage() const { return m_language; }
private:
  CURL           m_url;
  CFileItemList *m_items;
  std::string    m_language;
};

CGUIDialogSubtitles::CGUIDialogSubtitles(void)
    : CGUIDialog(WINDOW_DIALOG_SUBTITLES, "DialogSubtitles.xml")
    , m_subtitles(new CFileItemList)
    , m_serviceItems(new CFileItemList)
{
  m_loadType = KEEP_IN_MEMORY;
}

CGUIDialogSubtitles::~CGUIDialogSubtitles(void)
{
  CancelJobs();
  delete m_subtitles;
  delete m_serviceItems;
}

bool CGUIDialogSubtitles::OnMessage(CGUIMessage& message)
{
  if (message.GetMessage() == GUI_MSG_CLICKED)
  {
    int iControl = message.GetSenderId();
    bool selectAction = (message.GetParam1() == ACTION_SELECT_ITEM ||
                         message.GetParam1() == ACTION_MOUSE_LEFT_CLICK);

    bool contextMenuAction = (message.GetParam1() == ACTION_CONTEXT_MENU ||
                              message.GetParam1() == ACTION_MOUSE_RIGHT_CLICK);

    if (selectAction && iControl == CONTROL_SUBLIST)
    {
      CGUIMessage msg(GUI_MSG_ITEM_SELECTED, GetID(), CONTROL_SUBLIST);
      OnMessage(msg);

      int item = msg.GetParam1();
      if (item >= 0 && item < m_subtitles->Size())
        Download(*m_subtitles->Get(item));
      return true;
    }
    else if (selectAction && iControl == CONTROL_SERVICELIST)
    {
      CGUIMessage msg(GUI_MSG_ITEM_SELECTED, GetID(), CONTROL_SERVICELIST);
      OnMessage(msg);

      int item = msg.GetParam1();
      if (item >= 0 && item < m_serviceItems->Size())
      {
        SetService(m_serviceItems->Get(item)->GetProperty("Addon.ID").asString());
        Search();
      }
      return true;
    }
    else if (contextMenuAction && iControl == CONTROL_SERVICELIST)
    {
      CGUIMessage msg(GUI_MSG_ITEM_SELECTED, GetID(), CONTROL_SERVICELIST);
      OnMessage(msg);

      const int itemIdx = msg.GetParam1();
      if (itemIdx >= 0 && itemIdx < m_serviceItems->Size())
      {
        OnSubtitleServiceContextMenu(itemIdx);
      }
    }
    else if (iControl == CONTROL_MANUALSEARCH)
    {
      if (CGUIKeyboardFactory::ShowAndGetInput(m_strManualSearch, CVariant{g_localizeStrings.Get(24121)}, true))
      {
        Search(m_strManualSearch);
        return true;
      }
    }
  }
  else if (message.GetMessage() == GUI_MSG_WINDOW_DEINIT)
  {
    auto& components = CServiceBroker::GetAppComponents();
    const auto appPlayer = components.GetComponent<CApplicationPlayer>();
    if (appPlayer->IsPaused() && m_pausedOnRun)
      appPlayer->Pause();

    CGUIDialog::OnMessage(message);

    ClearSubtitles();
    ClearServices();
    return true;
  }
  return CGUIDialog::OnMessage(message);
}

void CGUIDialogSubtitles::OnInitWindow()
{
  m_pausedOnRun = false;
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          CSettings::SETTING_SUBTITLES_PAUSEONSEARCH) &&
      !appPlayer->IsPaused())
  {
    appPlayer->Pause();
    m_pausedOnRun = true;
  }

  FillServices();
  CGUIWindow::OnInitWindow();
  Search();
}

void CGUIDialogSubtitles::Process(unsigned int currentTime, CDirtyRegionList &dirtyregions)
{
  if (m_bInvalidated)
  {
    std::string status;
    CFileItemList subs;
    {
      std::unique_lock<CCriticalSection> lock(m_critsection);
      status = m_status;
      subs.Assign(*m_subtitles);
    }
    SET_CONTROL_LABEL(CONTROL_SUBSTATUS, status);

    if (m_updateSubsList)
    {
      CGUIMessage message(GUI_MSG_LABEL_BIND, GetID(), CONTROL_SUBLIST, 0, 0, &subs);
      OnMessage(message);
      if (!subs.IsEmpty())
      {
        CGUIMessage msg(GUI_MSG_SETFOCUS, GetID(), CONTROL_SUBLIST);
        OnMessage(msg);
      }
      m_updateSubsList = false;
    }

    int control = GetFocusedControlID();
    if (!control)
    {
      CGUIMessage msg(GUI_MSG_SETFOCUS, GetID(), m_subtitles->IsEmpty() ? CONTROL_SERVICELIST : CONTROL_SUBLIST);
      OnMessage(msg);
    }
    else if (control == CONTROL_SUBLIST && m_subtitles->IsEmpty())
    {
      CGUIMessage msg(GUI_MSG_SETFOCUS, GetID(), CONTROL_SERVICELIST);
      OnMessage(msg);
    }

    // Retry logic to fix subtitle freeze
    if (m_currentProvider && m_bSearching)
    {
      unsigned int elapsed = XbmcThreads::SystemClockMillis() - m_searchStartTime;

      if (elapsed > 3000 && m_subtitles->IsEmpty())
      {
        CLog::Log(LOGINFO, "SubtitleUnfreeze: Retry triggered");
        m_currentProvider->GetSubtitles(m_videoItem, m_language, *m_subtitles);
        m_searchStartTime = XbmcThreads::SystemClockMillis();
      }
    }
  }
  CGUIDialog::Process(currentTime, dirtyregions);
}

// (Rest of your original code remains unchanged)

