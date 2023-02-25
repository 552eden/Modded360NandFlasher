#include <Xtl.h>
#include "OutputConsole.h"
#include "AtgConsole.h"
#include "AtgInput.h"
#include "AtgUtil.h"
#include <time.h>
#include "Corona4G.h"
#include "Automation.h"
#include "keyextract.h"
#include <sstream>
#include <vector>
#include <string>
#include "AtgSignIn.h"
#include "ws-util.h"
#include <stdlib.h>
#include <iostream>

extern "C" {
#include "xenon_sfcx.h"
}

#pragma comment(lib, "xav")
#pragma comment(lib, "xapilib")
#pragma comment(lib,"Xnet.lib")
#include <winsockX.h>
#include <xbox.h>
#include <fstream>
#include <iostream>

#pragma comment(lib, "xboxkrnl.lib")
//#pragma comment(lib, "ws2_32.lib")

//#define SERVER_IP "10.100.102.18" // change this to the actual IP address of the server
#define SERVER_PORT 4343 // change this to the actual port number used by the server
#define FILENAME "game:\\flashdmp.bin"
#define SENTFILENAME "flashdmp.bin"
#define BUFFER_SIZE 4096



extern "C" NTSTATUS XeKeysGetKey(WORD KeyId, PVOID KeyBuffer, PDWORD keyLength);

ATG::GAMEPAD*	m_pGamepad; // Gamepad for input
time_t start,end; //Timer times for measuring time difference
double tdiff; // Double for time difference
bool started = false, dumped = false, dump_in_progress = false, MMC = false, write = false, AutoMode = false, GotSerial = false;
unsigned int config = 0;
BYTE consoleSerial[0xC];
XOVERLAPPED m_Overlapped;               // Overlapped object for message box UI
WCHAR           m_wstrMessage[256];         // Message box result message
MESSAGEBOX_RESULT m_Result; 
MESSAGEBOX_RESULT k_Result;// Message box button pressed result for sub folder detection
BOOL m_bMessageBoxShowing;
BOOL k_bMessageBoxShowing;
int deviceIndex;							//device chosen from message box. 0 = HDD, 1 = USB0 2 = USB1 
int subFolBool;								//use sub folder flag. 0 use 1 dont use
LPCWSTR g_pwstrButtons[3] =
{
    L"HDD", L"USB0", L"USB1"
};
LPCWSTR g_pwstrSubFolButtons[2] =
{
    L"Use sub folders", L"Dont use sub folders"
};


XDEVICE_DATA datastruct;
XCONTENTDEVICEID g_DeviceID;
PMESSAGEBOX_RESULT result = new MESSAGEBOX_RESULT();

char* subFolderChar;
char* mountLocation;
char* serverIp;
//char* subFolder = "";
//char* mountLocation = "\\Device\\Harddisk0\\Partition1\\";

extern "C" VOID HalReturnToFirmware(DWORD mode); // To Shutdown console when done ;)
extern "C" VOID XInputdPowerDownDevice(DWORD flag); // To Kill controllers
static unsigned long bswap32(unsigned long t) { return ((t & 0xFF) << 24) | ((t & 0xFF00) << 8) | ((t & 0xFF0000) >> 8) | ((t & 0xFF000000) >> 24); } //For ECC calculation
VOID KillControllers()
{
	XInputdPowerDownDevice(0x10000000);
	XInputdPowerDownDevice(0x10000001);
	XInputdPowerDownDevice(0x10000002);
	XInputdPowerDownDevice(0x10000003);
}

bool CheckPage(BYTE* page)
{
	unsigned int* data = (unsigned int*)page;
	unsigned int i=0, val=0, v=0;
	unsigned char edc[4];	
	for (i = 0; i < 0x1066; i++)
	{
		if (!(i & 31))
			v = ~bswap32(*data++);		
		val ^= v & 1;
		v>>=1;
		if (val & 1)
			val ^= 0x6954559;
		val >>= 1;
	}

	val = ~val;
	// 26 bit ecc data
	edc[0] = ((val << 6) | (page[0x20C] & 0x3F)) & 0xFF;
	edc[1] = (val >> 2) & 0xFF;
	edc[2] = (val >> 10) & 0xFF;
	edc[3] = (val >> 18) & 0xFF;
	return ((edc[0] == page[0x20C]) && (edc[1] == page[0x20D]) && (edc[2] == page[0x20E]) && (edc[3] == page[0x20F]));
}

int HasSpare(char* filename)
{
	BYTE buf[0x630];
	FILE* fd;
	dprintf(MSG_CHECKING_FOR_SPARE, filename);
	if (fopen_s(&fd, filename, "rb") != 0)
	{		
		dprintf(MSG_ERROR MSG_UNABLE_TO_OPEN_FOR_READING, filename);
		return -1;
	}
	if (fread_s(buf, 0x630, 0x630, 1, fd) != 1)
	{
		dprintf(MSG_ERROR MSG_UNABLE_TO_READ_0X630_BYTES_FROM, filename);
		fclose(fd);
		return -1;
	}
	fclose(fd);
	if (buf[0] != 0xFF && buf[1] != 0x4F)
	{
		dprintf(MSG_ERROR MSG_BAD_MAGIC, filename);
		return -1;
	}	
	for (int offset = 0; offset < 0x630; offset += 0x210)
	{
		if (CheckPage(&buf[offset]))
		{
			dprintf(MSG_SPARE_DETECTED, filename);
			return 0;
		}
	}
	dprintf(MSG_SPARE_NOT_DETECTED, filename);
	return 1;
}

void AutoCountdown(int timeout = 5)
{
	for (; timeout > 0; timeout--)
	{
		dprintf(MSG_YOU_HAVE_SECONDS_BEFORE_CONTINUE, timeout);
		Sleep(1000);
	}
	dprintf(MSG_TIMES_UP);
}

VOID flasher()
{
	if (!AutoMode)
	{
		dprintf(MSG_PRESS_START_TO_FLASH);
		for(;;)
		{
			m_pGamepad = ATG::Input::GetMergedInput();
			if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_START)
				break;
			else if (m_pGamepad->wPressedButtons)
				XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
		}
	}
	started = true;
	KillControllers();
	ClearConsole();
	time(&start);
	dprintf(MSG_WARNING_DO_NOT_TOUCH_CONSOLE_OR_CONTROLLER);
	int tmp = HasSpare("game:\\updflash.bin");
	if (tmp == -1)
		XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	bool isEcced = (tmp == 0);	
	if (!MMC)
	{
		if (!isEcced)
		{
			if (!AutoMode)
			{
				dprintf(MSG_WARNING_YOU_ARE_ABOUT_TO_FLASH_NO_SPARE_TO_SPARE_CONSOLE);
				for(;;)
				{
					m_pGamepad = ATG::Input::GetMergedInput();
					if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_START)
						break;
					else if (m_pGamepad->wPressedButtons)
						XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);				
				}
			}
			else
			{
				dprintf(MSG_WARNING_YOU_ARE_ABOUT_TO_FLASH_NO_SPARE_TO_SPARE_CONSOLE_AUTO);
				AutoCountdown();
			}
		}
		unsigned int r = sfcx_init();
		sfcx_printinfo(r);
#ifdef USE_UNICODE
		dprintf(L"\n\n");
#else
		dprintf("\n\n");
#endif
		try_rawflash("game:\\updflash.bin");
		sfcx_setconf(config);
	}
	else
	{
		if (isEcced)
		{
			if (!AutoMode)
			{
				dprintf(MSG_WARNING_YOU_ARE_ABOUT_TO_FLASH_SPARE_TO_NO_SPARE_CONSOLE);
				for(;;)
				{
					m_pGamepad = ATG::Input::GetMergedInput();
					if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_START)
						break;
					else if (m_pGamepad->wPressedButtons)
						XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
				}
			}
			else
			{
				dprintf(MSG_WARNING_YOU_ARE_ABOUT_TO_FLASH_SPARE_TO_NO_SPARE_CONSOLE_AUTO);
				AutoCountdown();
			}
		}
		try_rawflash4g("game:\\updflash.bin");
	}
	time(&end);
	tdiff = difftime(end,start);
	dprintf(MSG_COMPLETED_AFTER_SECONDS, tdiff);
	dprintf(MSG_REBOOTING_IN);
	for (int i = 5; i > 0; i--)
	{
#ifdef USE_UNICODE
		dprintf(L"%i", i);
#else
		dprintf("%i", i);
#endif
		for (int j = 0; j < 4; j++)
		{
			Sleep(250);
#ifdef USE_UNICODE
			dprintf(L".");
#else
			dprintf(".");
#endif
		}
	}
	dprintf(MSG_BYE);
	HalReturnToFirmware(2);
}


//---------------------- start network functionality ----------------------------
unsigned int computeChecksum(char* buffer, int length) {
    unsigned int checksum = 0;
    for (int i = 0; i < length; i++) {
        checksum += buffer[i];
    }
    return checksum;
}

// -------------------- END NETWORK FUNCTIONSLITY----------------

VOID dumper(char *filename)
{
	started = true;
	if (!MMC)
	{
		sfcx_init();
		sfcx_setconf(config);
		int size = sfc.size_bytes_phys;
		if((size == (RAW_NAND_64*4)) || (size == (RAW_NAND_64*8)))
		{
			if (!AutoMode)
			{
				dprintf(MSG_PRESS_A_TO_DUMP_SYSTEM_ONLY);
				dprintf(MSG_PRESS_B_TO_DUMP_FULL_NAND);
				dprintf(MSG_PRESS_BACK_TO_ABORT_DUMP);
				for(;;)
				{
					m_pGamepad = ATG::Input::GetMergedInput();
					if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_A)
					{
						size = RAW_NAND_64;
						break;
					}
					else if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_B)
						break;
					else if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_BACK)
						XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
					else if (m_pGamepad->wPressedButtons)
					{
						dprintf(MSG_TRY_AGAIN);
						dprintf(MSG_PRESS_A_TO_DUMP_SYSTEM_ONLY);
						dprintf(MSG_PRESS_B_TO_DUMP_FULL_NAND);
						dprintf(MSG_PRESS_BACK_TO_ABORT_DUMP);
					}
				}
			}
			else
			{
				if (size == (RAW_NAND_64*4))
					size = 256;
				else
					size = 512;
				dprintf(MSG_BB_DETECTED_SETTING_64MB, size);
				size = RAW_NAND_64;
			}
		}
		ClearConsole();
		time(&start);
		unsigned int r = sfcx_init();
		sfcx_printinfo(r);
		try_rawdump(filename, size);
		sfcx_setconf(config);
	}
	else
	{
		ClearConsole();
		time(&start);
		try_rawdump4g(filename);
	}
	time(&end);
	tdiff = difftime(end,start);
	dprintf(MSG_COMPLETED_AFTER_SECONDS, tdiff);
	dumped = true;
}

void PrintExecutingCountdown(int max)
{
	for (; max > 0; max--)
	{
		dprintf(MSG_EXECUTING_COMMAND_IN_SECONDS, max);
		Sleep(1000);
	}
	dprintf(MSG_EXECUTING_COMMAND);
}

void TryAutoMode()
{
	dprintf(MSG_LOOKING_FOR_CMD_FILE_FOR_AUTO_MODE);
	if (fexists("game:\\simpleflasher.cmd"))
	{
		AutoMode = true;
		dprintf(MSG_SIMPLEFLASHER_CMD_FOUND_ENTERING_AUTO);
		int mode = CheckMode("game:\\simpleflasher.cmd");
		if (mode == 1) //AutoDump
		{
			dprintf(MSG_AUTO_DUMP_FOUND);
			dumper("game:\\flashdmp.bin");
			GenerateHash("game:\\flashdmp.bin");
		}
		else if (mode == 2) //AutoFlash
		{
			dprintf(MSG_AUTO_FLASH_FOUND);
			if (CheckHash("game:\\updflash.bin"))
			{
				PrintExecutingCountdown(5);
				flasher();
			}
			else
				dprintf(MSG_ERROR MSG_HASH_DONT_MATCH);
		}
		else if (mode == 3) //AutoSafeFlash
		{
			dprintf(MSG_AUTO_SAFE_FLASH_FOUND);
			if (CheckHash("game:\\updflash.bin"))
			{
				PrintExecutingCountdown(5);
				dumper("game:\\recovery.bin");
				AutoCountdown();
				flasher();
			}
			else
				dprintf(MSG_ERROR MSG_HASH_DONT_MATCH);
		}
		else if (mode == 4) //AutoExit, only want key...
		{
			dprintf(MSG_AUTO_EXIT_FOUND);
			PrintExecutingCountdown(5);
		}
		else if (mode == 5) //AutoReboot Hard Reset
		{
			dprintf(MSG_AUTO_REBOOT_FOUND);
			PrintExecutingCountdown(5);
			HalReturnToFirmware(2);
		}
		else
		{
			dprintf(MSG_BAD_COMMAND_FILE_RETURNING_TO_MANUAL_MODE);
			AutoMode = false;
			return;
		}
		if (AutoMode)
			XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0); //We don't want to return to manual mode ;)
	}
	else
	{
		dprintf(MSG_COMMAND_FILE_NOT_FOUND_ENTERING_MANUAL_MODE);
	}
}

void PrintConsoleInfo(bool GotKey)
{
	dprintf(MSG_CONSOLE_INFO_LINE);
	PrintDash();
	PrintDLVersion();
	if (GotKey)
		PrintCPUKey();
	if (GotSerial)
		dprintf(MSG_CONSOLE_SERIAL, consoleSerial);
	dprintf(MSG_CONSOLE_INFO_BOTTOM);
}

void InfoBox(LPCWSTR Text, LPCWSTR Caption) {
	LPCWSTR* text;
	text = new LPCWSTR[1];
	text[0] = L"Ok";
	PXOVERLAPPED over = new XOVERLAPPED();
	memset(over, 0, sizeof(over));
	PMESSAGEBOX_RESULT result = new MESSAGEBOX_RESULT();
	memset(result, 0, sizeof(result));

	while(XShowMessageBoxUI(0, Caption, Text, 1, text, 0, XMB_ERRORICON, result, over) == ERROR_ACCESS_DENIED)
		Sleep(500);
	while(!XHasOverlappedIoCompleted(over))
		Sleep(500);
}


bool CheckGameMounted(std::string storageDevice) {
	if(storageDevice != "game")
	{
		FILE * fd;
		mount("game:", mountLocation);
		
		if (fopen_s(&fd, "game:\\test.tmp", "w") != 0)
		{
			dprintf("\nfolder dosent exist! please restart wih a folder that exists!");
			InfoBox(L"folder dosent exist! please restart wih a folder that exists!", L"Nand Flasher Error");
			Sleep(100);
			return false;
		}
		else
		{
			fclose(fd);
			remove("game:\\test.tmp");
		}
		return true;
	}
	else
	{
		FILE * fd;
		if (fopen_s(&fd, "game:\\test.tmp", "w") != 0)
		{
			dprintf("Error! if you chose to use xex folder then use another option");
			InfoBox(L"Error! if you chose to use xex folder then use another option", L"Nand Flasher Error");
			Sleep(100);
			return false;
		}
		else
		{
			fclose(fd);
			remove("game:\\test.tmp");
		}
		return true;
	}
	
}


std::string getPathFromKeyboard()
{
    XOVERLAPPED Overlapped;
    WCHAR GTTEXT[512];
    char Buffer[512];
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    XShowKeyboardUI(0, VKBD_DEFAULT, L"", L"Nand Flasher", L"Please enter directory to read/write. For subfolders use backslash(\\). Press B to use root of selected drive", GTTEXT, 512, &Overlapped);
    while (!XHasOverlappedIoCompleted(&Overlapped))
		Sleep(1000);
    std::wstring ws(GTTEXT);
	std::string rtn( ws.begin(), ws.end() );
	return rtn;
    
}

std::string getIPFromKeyboard()
{
    XOVERLAPPED Overlapped;
    WCHAR GTTEXT[512];
    char Buffer[512];
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    XShowKeyboardUI(0, VKBD_DEFAULT, L"", L"Nand Flasher", L"ENTER PC SERVER IP (EG. 192.168.1.12)", GTTEXT, 512, &Overlapped);
    while (!XHasOverlappedIoCompleted(&Overlapped))
		Sleep(1000);
    std::wstring ws(GTTEXT);
	std::string rtn( ws.begin(), ws.end() );
	return rtn;
    
}


VOID ShowMessageBoxUI() //shows drive selector message box
{
    DWORD dwRet;

    ZeroMemory( &m_Overlapped, sizeof( XOVERLAPPED ) );

    dwRet = XShowMessageBoxUI( 0,
                               L"Select Storage Device",                   // Message box title
                               L"Please select a storage devce to read/write, *press B to use XEX folder*",  // Message string
                               ARRAYSIZE( g_pwstrButtons ),// Number of buttons
                               g_pwstrButtons,             // Button captions
                               0,                          // Button that gets focus
                               XMB_ALERTICON,              // Icon to display
                               &m_Result,                  // Button pressed result
                               &m_Overlapped );

    assert( dwRet == ERROR_IO_PENDING );

    m_bMessageBoxShowing = TRUE;
    m_wstrMessage[0] = L'\0';
}

VOID ShowMessageBoxUISubFol()//shows sub folder message box
{
    DWORD dwRet;

    ZeroMemory( &m_Overlapped, sizeof( XOVERLAPPED ) );

    dwRet = XShowMessageBoxUI( 0,
                               L"Use a subfolder?",                   // Message box title
                               L"Do you want to read/write using a subfolder? (Make sure the folder exists!)",  // Message string
                               ARRAYSIZE( g_pwstrSubFolButtons ),// Number of buttons
							   g_pwstrSubFolButtons,             // Button captions
                               0,                          // Button that gets focus
							   XMB_ALERTICON,              // Icon to display
                               &k_Result,                  // Button pressed result
                               &m_Overlapped );

    assert( dwRet == ERROR_IO_PENDING );

    k_bMessageBoxShowing = TRUE;
    m_wstrMessage[0] = L'\0';
}


//---------------------- network functionality -------------------


//// Constants /////////////////////////////////////////////////////////

// Default port to connect to on the server
const int kDefaultServerPort = 4242;

//---------------------- end network functionality ---------------

//--------------------------------------------------------------------------------------
// Name: main()
// Desc: Entry point to the program
//--------------------------------------------------------------------------------------
VOID __cdecl main()
{
	
	// Initialize the console window
	MakeConsole("embed:\\font", CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GREEN);
	std::string storageDevice;
	Sleep(1000);
	ShowMessageBoxUI();
	while(m_bMessageBoxShowing)
	{
		
		while(!XHasOverlappedIoCompleted( &m_Overlapped ))//check if message box is closed yet
		{
			Sleep(100);
		}
		 if( XHasOverlappedIoCompleted( &m_Overlapped ) )//message box ended
        {
            m_bMessageBoxShowing = FALSE;
            DWORD dwResult = XGetOverlappedResult( &m_Overlapped, NULL, TRUE );
            if( dwResult == ERROR_SUCCESS )
            {
                deviceIndex = m_Result.dwButtonPressed;//remember which buttom was pressed
            }
            else if( dwResult == ERROR_CANCELLED )
            {
                deviceIndex = 3;
				dprintf("Messagebox cancelled, defaulting to XEX location");
            }
            else
            {
                deviceIndex = 3;
				dprintf("Messagebox error, defaulting to XEX location");
            }

        }
	}
	Sleep(1000);



       
    


	
	dprintf("Welcome to Modded360nandflasher. a mod for Simple360nandflasher which adds folder choice option!\n\n");

	if(deviceIndex == 0)
	{
		storageDevice = "\\Device\\Harddisk0\\Partition1\\";
	}
	else if(deviceIndex == 1)
	{
		storageDevice = "\\Device\\Mass0\\";
	}
	else if(deviceIndex == 2)
	{
		storageDevice = "\\Device\\Mass1\\";
	}
	else
	{
		deviceIndex = 3;
		storageDevice = "game";
	}

	ShowMessageBoxUISubFol();//show sub folder question
	Sleep(1000);
	while(k_bMessageBoxShowing)
	{
		
		while(!XHasOverlappedIoCompleted( &m_Overlapped ))
		{
			
		}
		 if( XHasOverlappedIoCompleted( &m_Overlapped ) )
        {
            k_bMessageBoxShowing = FALSE;
            DWORD dwResult = XGetOverlappedResult( &m_Overlapped, NULL, TRUE );
            if( dwResult == ERROR_SUCCESS )
            {
				subFolBool = k_Result.dwButtonPressed;
            }
            else if( dwResult == ERROR_CANCELLED )
            {
				subFolBool = 1;
				dprintf("Messagebox cancelled, defaulting to XEX location");
            }
            else
            {
                subFolBool = 1;
				dprintf("Messagebox error, defaulting to XEX location");
            }

        }
	}
	Sleep(1000);

	if(subFolBool == 0)//if you want a sub folder
	{
		std::string subFolder =  getPathFromKeyboard();
		std::string stringMountLocation = storageDevice + subFolder;
		mountLocation = const_cast<char*>(stringMountLocation.c_str());
	}
	else
	{
		
		mountLocation = const_cast<char*>(storageDevice.c_str());
	}

	
	

	if (!CheckGameMounted(storageDevice))//check if the folder even exists
	{
		return;
	}

	write = fexists("game:\\updflash.bin");
	
	
	

	
#ifdef TRANSLATION_BY
#ifdef USE_UNICODE
	dprintf(L"Simple 360 NAND Flasher by Swizzy v1.5 (BETA)\n");
#else
	dprintf("Simple 360 NAND Flasher by Swizzy v1.5 (BETA)\n");
#endif
	dprintf(TRANSLATION_BY);
#else
	dprintf("Simple 360 NAND Flasher by Swizzy v1.5 (BETA)\n");
#endif
	dprintf("Mod By 552eden\n");
	dprintf("*********Mount location is:");
	dprintf(mountLocation," *********\n");
	dprintf(MSG_DETECTING_NAND_TYPE);
	
	MMC = (sfcx_detecttype() == 1); // 1 = MMC, 0 = RAW NAND
	if (!MMC)
		config = sfcx_getconf();
	bool GotKey = false;
	dprintf(MSG_ATTEMTPING_TO_GRAB_CPUKEY);
	if (GetCPUKey())
	{
		SaveCPUKey("game:\\cpukey.txt");
		GotKey = true;
	}
	else
	{
		dprintf(MSG_ERROR MSG_INCOMPATIBLE_DASHLAUNCH);
		GotKey = false;
	}
	dprintf(MSG_ATTEMPTING_TO_GET_CONSOLE_SERIAL);
	DWORD dwtmp = 0xC;
	GotSerial = XeKeysGetKey(0x14, consoleSerial, &dwtmp) >= 0;
	PrintConsoleInfo(GotKey);
	TryAutoMode();
	if (write)
	{
		if (!MMC)
		{
			dprintf(MSG_PRESS_A_TO_FLASH_RAWFLASH);
			dprintf(MSG_PRESS_B_TO_SAFE_FLASH_RAWFLASH);
		}
		else
		{
			dprintf(MSG_PRESS_A_TO_FLASH_RAWFLASH4G);
			dprintf(MSG_PRESS_B_TO_SAFE_FLASH_RAWFLASH4G);
		}
	}
	if (!MMC)
	{
		dprintf(MSG_PRESS_X_TO_DUMP_RAWFLASH);
		dprintf("\npress select to use network beta\n");
	}
	else
		dprintf(MSG_PRESS_X_TO_DUMP_RAWFLASH4G);
	dprintf(MSG_PRESS_ANY_OTHER_BUTTON_TO_EXIT);
	
	for(;;)
	{
		m_pGamepad = ATG::Input::GetMergedInput();
		if (!started)
		{
			if ((m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_A) && (write))
			{
				flasher();
			}
			else if ((m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_B) && (!dumped) && (write))
			{
				if (!fexists("game:\\recovery.bin"))
					dumper("game:\\recovery.bin");
				else
				{
					dprintf(MSG_PRESS_START_TO_OVERWRITE_EXISTING_FILE, "game:\\recovery.bin");
					if (GotSerial)
						dprintf(MSG_PRESS_B_TO_OVERWRITE_EXISTING_FILE_SERIAL, "game:\\recovery", consoleSerial);
					for(;;)
					{
						m_pGamepad = ATG::Input::GetMergedInput();
						if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_START)
						{
							dumper("game:\\recovery.bin");
							break;
						}
						else if ((m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_B) && GotSerial)
						{
							char path[512];
							sprintf_s(path, 512, "game:\\recovery_%s.bin", consoleSerial);							
							dumper(path);
							break;
						}
					}
				}
				flasher();
			}
			else if ((m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_X) && (!dumped))
			{
				if (!fexists("game:\\flashdmp.bin"))
					dumper("game:\\flashdmp.bin");
				else
				{
					dprintf(MSG_PRESS_START_TO_OVERWRITE_EXISTING_FILE, "game:\\flashdmp.bin");
					if (GotSerial)
						dprintf(MSG_PRESS_B_TO_OVERWRITE_EXISTING_FILE_SERIAL, "game:\\flashdmp", consoleSerial);
					for(;;)
					{
						m_pGamepad = ATG::Input::GetMergedInput();
						if (m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_START)
						{
							dumper("game:\\flashdmp.bin");
							break;
						}
						else if ((m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_B) && (GotSerial))
						{
							char path[512];
							sprintf_s(path, 512, "game:\\flashdmp_%s.bin", consoleSerial);							
							dumper(path);
							break;
						}
					}
				}
				dprintf(MSG_PRESS_ANY_BUTTON_TO_EXIT);
				dprintf("\npress select to use network beta\n");
			}
			else if ((m_pGamepad->wPressedButtons & XINPUT_GAMEPAD_BACK) && (!dumped)) //network func button
			{
				dprintf("network beta activated \î");

				//disable secure network settings. long live unsecure connections!
				XNetStartupParams xnsp;
				memset( &xnsp, 0, sizeof( xnsp ) );
				xnsp.cfgSizeOfStruct = sizeof( XNetStartupParams );
				xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
				INT err = XNetStartup( &xnsp );
				std::string ipStr = getIPFromKeyboard();
				serverIp = const_cast<char*>(ipStr.c_str());
				//dprintf("\n server IP is: ", serverIp, "\n");
				// Initialize WinsockX
				WSADATA wsaData;
				int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
				if (result != 0) {
					printf("WSAStartup failed with error: %d\n", result);
					//return 1;
				}

				// Create a socket
				SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (sock == INVALID_SOCKET) {
					dprintf("socket creation failed with error: %d\n", WSAGetLastError());
					WSACleanup();
					//return 1;
				}
				else
				{
					dprintf("Socket created successfully!\n");
				}

				// Connect to the server
				SOCKADDR_IN target;
				target.sin_family = AF_INET;
				target.sin_port = htons(SERVER_PORT);
				target.sin_addr.s_addr = inet_addr(serverIp);
				result = connect(sock, (SOCKADDR*)&target, sizeof(target));
				if (result == SOCKET_ERROR) {
					printf("connect failed with error: %d\n", WSAGetLastError());
					closesocket(sock);
					WSACleanup();
					//return 1;
				}
				else
				{
					dprintf("Connected to PC server!\n");
				}

				printf("Connected to server %s:%d\n", serverIp, SERVER_PORT);

				// Send the name of the binary file to receive
				result = send(sock, SENTFILENAME, strlen(SENTFILENAME), 0);
				if (result == SOCKET_ERROR) {
					printf("send failed with error: %d\n", WSAGetLastError());
					closesocket(sock);
					WSACleanup();
					//return 1;
				}

				// Receive the expected file size
				char sizeBuffer[1024];
				result = recv(sock, sizeBuffer, 1024, 0);
				//dprintf("recieved file size from server is:",result);
				if (result == SOCKET_ERROR) {
					printf("recv failed with error: %d\n", WSAGetLastError());
					closesocket(sock);
					WSACleanup();
					//return 1;
				}
				sizeBuffer[result] = '\0';
				long long expectedFileSize = _atoi64(sizeBuffer);
				printf("Expected file size: %lld\n", expectedFileSize);

				// Open the file for writing
				std::ofstream outputFile(FILENAME, std::ios::out | std::ios::binary);
				if (!outputFile.is_open()) {
					dprintf("Failed to create file %s\n", FILENAME);
					closesocket(sock);
					WSACleanup();
					//return 1;
				}

				// Receive the file and compute the checksum
				char buffer[BUFFER_SIZE];
				unsigned int checksum = 0;
				int bytesRead;
				long long totalBytesRead = 0;
				while (totalBytesRead < expectedFileSize) {
					bytesRead = recv(sock, buffer, BUFFER_SIZE, 0);
					if (bytesRead <= 0) {
						break;
					}
					outputFile.write(buffer, bytesRead);
					checksum += computeChecksum(buffer, bytesRead);
					totalBytesRead += bytesRead;
				}

				// Check for errors
				if (bytesRead == SOCKET_ERROR) {
					printf("recv failed with error: %d\n", WSAGetLastError());
				}
				else {
					dprintf("\nFile received successfully\n");
					dprintf("File received successfully\n");
					dprintf("File received successfully\n");
					/*
					if (expectedFileSize != totalBytesRead) {
						printf("Received file size %lld is different from expected file size %lld\n", totalBytesRead, expectedFileSize);
					}
					else {
						// Compare the expected checksum to the actual checksum
						if (checksum == expectedChecksum) {
							printf("Checksums match, file received successfully\n");
						}
						else {
							printf("Checksums do not match, file may be corrupted\n");
						}
					}*/
				}


				// Close the file, socket and cleanup WinsockX
				outputFile.close();


				closesocket(sock);
				WSACleanup();
				dprintf("\n network func completed!!\n");
				dprintf("network func completed!!\n");
				dprintf("network func completed!!\n");
				dprintf("network func completed!!\n");


				
			}


			
			else if (m_pGamepad->wPressedButtons) { break; }
		}
		else if ((m_pGamepad->wPressedButtons) && (dumped)) { break; }
	}
	if (!MMC)
		sfcx_setconf(config);
}