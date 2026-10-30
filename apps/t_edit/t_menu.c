/* KolibriOS for ARM: t_edit's menu bar, translated to C from
 * programs/other/t_edit/t_menu.inc (IgorA, the KolibriOS team;
 * GPL-2.0), with lib/kmenu (kmenu.obj's functions); English, as t_edit's
 * en_US build. The items give the toolbar's buttons' identifiers. */
#include "t_edit.h"

static const char sz_main_menu_File[] = "File";
static const char sz_main_menu_File_New[] = "New";
static const char sz_main_menu_File_Open[] = "Open";
static const char sz_main_menu_File_Save[] = "Save";
static const char sz_main_menu_File_Save_As[] = "Save as";
static const char sz_main_menu_File_Exit[] = "Exit";

static const char sz_main_menu_Changes[] = "Changes";
static const char sz_main_menu_Changes_Undo[] = "Undo";
static const char sz_main_menu_Changes_Redo[] = "Redo";

static const char sz_main_menu_Buf[] = "Buffer";
static const char sz_main_menu_Buf_Cut[] = "Cut";
static const char sz_main_menu_Buf_Copy[] = "Copy";
static const char sz_main_menu_Buf_Paste[] = "Paste";

static const char sz_main_menu_Search[] = "Search";
static const char sz_main_menu_Search_Text[] = "Text search";
static const char sz_main_menu_Search_Keywords[] = "Search in keywords";

static const char sz_main_menu_View[] = "View";
static const char sz_main_menu_Veiw_UseSyntax[] = "Use syntax highlight";
static const char sz_main_menu_Veiw_ChooseSyntax[] = "Choose syntax highlight";
static const char sz_main_menu_Veiw_LineBreak[] = "Show line break symbols";

static const char sz_main_menu_Encoding[] = "Encoding";
static const char sz_main_menu_Encoding_1251to866[] = "CP1251 \x1a CP866";
static const char sz_main_menu_Encoding_866to1251[] = "CP866 \x1a CP1251";

ksubmenu_t *main_menu;
static ksubmenu_t *main_menu_file, *main_menu_changes, *main_menu_buf, *main_menu_search, *main_menu_view,
	*main_menu_encoding;

void tedit_menu_init(void)
{
	kmenu_init(&sc);

	main_menu = ksubmenu_new();

	main_menu_file = ksubmenu_new();
	ksubmenu_add(main_menu_file, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_File_New, ID_BUT_NEW));
	ksubmenu_add(main_menu_file, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_File_Open, ID_BUT_OPEN));
	ksubmenu_add(main_menu_file, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_File_Save, ID_BUT_SAVE));
	ksubmenu_add(main_menu_file, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_File_Save_As, ID_BUT_SAVE_AS));
	ksubmenu_add(main_menu_file, kmenuitem_new(KMENUITEM_SEPARATOR, 0, 0));
	ksubmenu_add(main_menu_file, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_File_Exit, 199));
	ksubmenu_add(main_menu, kmenuitem_new(KMENUITEM_SUBMENU, sz_main_menu_File, (u32)main_menu_file));

	main_menu_changes = ksubmenu_new();
	ksubmenu_add(main_menu_changes, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Changes_Undo, ID_BUT_UNDO));
	ksubmenu_add(main_menu_changes, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Changes_Redo, ID_BUT_REDO));
	ksubmenu_add(main_menu, kmenuitem_new(KMENUITEM_SUBMENU, sz_main_menu_Changes, (u32)main_menu_changes));

	main_menu_buf = ksubmenu_new();
	ksubmenu_add(main_menu_buf, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Buf_Cut, ID_BUT_CUT));
	ksubmenu_add(main_menu_buf, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Buf_Copy, ID_BUT_COPY));
	ksubmenu_add(main_menu_buf, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Buf_Paste, ID_BUT_PASTE));
	ksubmenu_add(main_menu, kmenuitem_new(KMENUITEM_SUBMENU, sz_main_menu_Buf, (u32)main_menu_buf));

	main_menu_search = ksubmenu_new();
	ksubmenu_add(main_menu_search, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Search_Text, ID_BUT_FIND));
	ksubmenu_add(main_menu_search, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Search_Keywords, ID_BUT_KEY_WORDS));
	ksubmenu_add(main_menu, kmenuitem_new(KMENUITEM_SUBMENU, sz_main_menu_Search, (u32)main_menu_search));

	main_menu_view = ksubmenu_new();
	ksubmenu_add(main_menu_view, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Veiw_UseSyntax, ID_BUT_SYNTAX_LIST));
	ksubmenu_add(main_menu_view, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Veiw_ChooseSyntax, ID_BUT_SYNTAX_MODE));
	ksubmenu_add(main_menu_view, kmenuitem_new(KMENUITEM_SEPARATOR, 0, 0));
	ksubmenu_add(main_menu_view, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Veiw_LineBreak, ID_BUT_INVISIBLE));
	ksubmenu_add(main_menu, kmenuitem_new(KMENUITEM_SUBMENU, sz_main_menu_View, (u32)main_menu_view));

	main_menu_encoding = ksubmenu_new();
	ksubmenu_add(main_menu_encoding, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Encoding_1251to866, ID_BUT_CONVERT_1251_866));
	ksubmenu_add(main_menu_encoding, kmenuitem_new(KMENUITEM_NORMAL, sz_main_menu_Encoding_866to1251, ID_BUT_CONVERT_866_1251));
	ksubmenu_add(main_menu, kmenuitem_new(KMENUITEM_SUBMENU, sz_main_menu_Encoding, (u32)main_menu_encoding));
}
