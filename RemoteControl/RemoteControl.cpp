#define _WINSOCK_DEPRECATED_NO_WARNINGS //告诉编译器我使用了被标记为废弃状态的老API函数，不要给我提示警告
#include <stdio.h>
#include<Windows.h> //window编程
#include<atlimage.h> //提供轻量级的位图/DIB 加载、显示、保存能力

#pragma comment(lib,"ws2_32.lib") //链接库
#define RECV_BUFFER_SIZE (1024 * 1024 * 10) //10MB缓冲

//将这个结构体按照1字节对齐  这样就不会有内存对齐的填充字节  结构体的大小就是成员变量的大小之和
#pragma pack(push,1)
struct PacketHeader
{
	int magic; //4字节包头
	int cmd;   //4字节命令号
	int body_len; //数据长度
};
#pragma pack(pop) //保证只影响PacketHeader结构体  不污染后面的代码

struct Packet
{
	PacketHeader header; //包头
	char body[]; //包体  柔性数组  不占结构体大小  需要动态分配内存  body 指向 header 之后的内存
};


//暗号表
//鼠标信息  按键：右键、左键、中间   状态：按下、放开、双击
enum ENUM_MOUSE{
	MOUSE_MOVE = 1, //鼠标移动
	MOUSE_LDOWN = 2, //鼠标左键按下
	MOUSE_LUP = 3, //鼠标左键抬起
	MOUSE_RDOWN = 4, //鼠标右键按下
	MOUSE_RUP = 5, //鼠标右键抬起
	MOUSE_MDOWN = 6,//鼠标中键按下
	MOUSE_MUP = 7, //鼠标中键抬起
	MOUSE_LCLICK = 8,//左键单击
	MOUSE_RCLICK = 9, //右键单击
	MOUSE_MCLICK = 10,//中键单击
	MOUSE_LDCLICK = 11,//鼠标左键双击
	MOUSE_RDCLICK = 12,//鼠标右键双击
	MOUSE_MDCLICK = 13,//鼠标中键双击

};

struct Mouse {
	int action; // 鼠标的行为
	POINT ptXY; // 鼠标的坐标 x,y
};

struct KeyBoard {
	int virtual_code;//虚拟码
	int key_status;//按下/放开
};

//方便定义整数常量
enum CMD
{
	CMD_SCREEN = 1,
	CMD_MOUSE = 2,
	CMD_TESTCONNECT = 200,
	CMD_KEYBOARD = 4
};


Packet* ParsePacket(char* buffer, int len);
Packet* PackPacket(int cmd, char* buffer, int buffer_len);
int GetPacketLen(Packet* pck);
int InitServer();
int HandleCommand(Packet* packet);

int HandleScreen(Packet* packet);
int HandleMouse(Packet* packet);
int HandleKeyBoard(Packet* packet);
int HandleTestConnect(Packet* packet);


SOCKET g_server_socket;  //监听套接字  用于等待客户端连接
SOCKET g_client_socket;  //客户端套接字  用于和客户端通信

unsigned long g_screen_thread_id = 0;  // 三个工作线程的 ID，用于 PostThreadMessage  收件地址
unsigned long g_mouse_thread_id = 0;
unsigned long g_keyboard_thread_id = 0;

//自定义消息  WM_USER 以上留给应用
#define WM_HANDLE_SCREEN (WM_USER + 1)
#define WM_HANDLE_MOUSE (WM_USER + 2)
#define WM_HANDLE_KEYBOARD (WM_USER + 3)
#define WM_HANDLE_INVOKE_MSG_LOOP (WM_USER + 4)  //专门用来"激活"消息队列的空消息

//DWORD:Double Word 32位无符号整数  WINAPI:调用约定
DWORD WINAPI HandleScreenThreadFuc(LPVOID lpThreadParameter)
{
	//主线程 CreateThread 后立即 PostThreadMessage 时线程可能还没执行到 GetMessage，消息会丢。 Sleep(100) 兜底
	MSG msg;
	while (GetMessage(&msg, 0, 0, 0))//阻塞等待消息  收到返回非0进入循环  WM_QUIT返回0  
	{//每个线程有自己的消息队列，靠 thread_id 定位
		if (msg.message == WM_HANDLE_SCREEN)
		{
			Packet* packet = (Packet*) msg.lParam;  // 主线程把 Packet* 塞在 lParam 里传过来  lParam传指针
			HandleScreen(packet);  //真正干活的函数
			free(packet);  //释放内存
		}
	}
	return 0;
}

DWORD WINAPI HandleMouseThreadFuc(LPVOID lpThreadParameter)
{
	MSG msg;
	while (GetMessage(&msg, 0, 0, 0))
	{
		if (msg.message == WM_HANDLE_MOUSE)
		{
			Packet* packet = (Packet*)msg.lParam;
			HandleMouse(packet);
			free(packet);
		}
	}
	return 0;
}

DWORD WINAPI HandleKeyBoardThreadFuc(LPVOID lpThreadParameter)
{
	MSG msg;
	while (GetMessage(&msg, 0, 0, 0))
	{
		if (msg.message == WM_HANDLE_KEYBOARD)
		{
			Packet* packet = (Packet*)msg.lParam;
			HandleKeyBoard(packet);
			free(packet);
		}
	}
	return 0;
}

//服务端
int main()
{
	SetProcessDPIAware();//设置当前进程的DPI感知上下文  DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:每个显示器都具有独立的DPI感知能力 适用于多显示器环境
	if (InitServer() != 0)
	{
		printf("启动服务失败！\n");
		return 0;
	}

	//为每个命令当独开一条流水线
	CreateThread(NULL,0, HandleScreenThreadFuc,NULL,0,&g_screen_thread_id);
	CreateThread(NULL, 0, HandleMouseThreadFuc, NULL, 0, &g_mouse_thread_id);
	CreateThread(NULL, 0, HandleKeyBoardThreadFuc, NULL, 0, &g_keyboard_thread_id);

	//WM_HANDLE_INVOKE_MSG_LOOP
	//往线程里面提前post一个消息，用于激活消息队列
	PostThreadMessage(g_mouse_thread_id, WM_HANDLE_INVOKE_MSG_LOOP, NULL, NULL);
	PostThreadMessage(g_screen_thread_id, WM_HANDLE_INVOKE_MSG_LOOP, NULL, NULL);
	PostThreadMessage(g_keyboard_thread_id, WM_HANDLE_INVOKE_MSG_LOOP, NULL, NULL);
	Sleep(100);


	//5.等待客户端连接 会返回客户端的socket  用于和客户端通信
	SOCKADDR_IN client_addr;
	int client_addr_size = sizeof(client_addr);
	printf("等待客户端连接...\n");
	//accept函数会阻塞当前线程 直到有客户端连接到服务器  返回一个新的套接字  用于和客户端通信
	g_client_socket = accept(g_server_socket, (SOCKADDR*)&client_addr, &client_addr_size); //等待客户端连接  返回一个新的套接字  用于和客户端通信
	printf("客户端连接成功！\n");

	//6.等待客户端发送数据
	char* buffer = (char*)malloc(RECV_BUFFER_SIZE);

	//记录缓冲区当前数据的长度
	static int index = 0;
	while (true)//循环接受数据
	{

		if (index >= RECV_BUFFER_SIZE)
		{
			printf("接收缓冲区已满，可能收到大量无效数据，退出。\n");
			break;
		}

		int len = recv(g_client_socket, buffer + index, RECV_BUFFER_SIZE - index, 0); //等待客户端发送数据  阻塞函数
		if (len > 0) {
			index += len;
			printf("接收数据成功：%d！\n",len);
		}
		else
		{
			
			break;

		}

		//鼠标请求  
		if (index > 0)
		{

			printf("开始解析数据！\n");
			Packet* packet = ParsePacket(buffer, index); // 解析数据
			while (packet != NULL && index > 0)
			{
				
				printf("解析数据成功！\n");
				
				index -= GetPacketLen(packet);
				memmove(buffer, buffer + GetPacketLen(packet), index);

				HandleCommand(packet);//处理命令
				
				packet = ParsePacket(buffer, index);
			}
		}

	}
	closesocket(g_client_socket); //关闭客户端socket
	closesocket(g_server_socket); //关闭服务器socket
	//8.释放网络服务器
	WSACleanup();

	//HandleScreen(NULL);
    return 0;
}

int HandleCommand(Packet* packet)
{
	printf("Handle cmd:%d\r\n",packet->header.body_len);
	int ret = 0;
	switch (packet->header.cmd)
	{

	case CMD_SCREEN://发送屏幕命令
		//向线程投递消息是异步的，瞬间执行不阻塞
		if(!PostThreadMessage(g_screen_thread_id,WM_HANDLE_SCREEN,NULL,(LPARAM)packet))
		{
			free(packet);
		}
		//ret = HandleScreen(packet);
		break;

	case CMD_MOUSE://鼠标事件
		if(!PostThreadMessage(g_mouse_thread_id, WM_HANDLE_MOUSE, NULL, (LPARAM)packet))
		{
			free(packet);
		}
		//ret = HandleMouse(packet);
		break;

	case CMD_KEYBOARD://键盘命令
		if(!PostThreadMessage(g_keyboard_thread_id, WM_HANDLE_KEYBOARD, NULL, (LPARAM)packet))
		{
			free(packet);
		}
		//ret = HandleKeyBoard(packet);
		break;
	
	case CMD_TESTCONNECT://测试连接命令
		ret = HandleTestConnect(packet);
		free(packet);
		break;

	default:
		free(packet);
		break;
	}

	return ret;
}

int HandleScreen(Packet* packet)
{
	//获取本地屏幕

	CImage image;//创建一个image对象

	HDC hScreen = GetDC(NULL);//获取屏幕DC  NULL=整个屏幕

	int bitWidth = GetDeviceCaps(hScreen, BITSPIXEL);//获取屏幕的位宽
	printf("bitWidth: %d\n", bitWidth);
	
	


	int sWidth = GetSystemMetrics(SM_CXSCREEN);//获取屏幕宽度
	int sHeight = GetSystemMetrics(SM_CYSCREEN);//获取屏幕高度
	printf("sWidth: %d, sHeight: %d\n", sWidth, sHeight);

	image.Create(sWidth, sHeight, bitWidth);//创建一个和屏幕一样大小的图像对象

	BitBlt(image.GetDC(), 0, 0, sWidth, sHeight, hScreen, 0, 0, SRCCOPY);//将屏幕数据复制到图像对象中

	ReleaseDC(NULL,hScreen);//释放屏幕上下文

	//转为网络流
	//image.Save("test.png",::Gdiplus::ImageFormatPNG);
	HGLOBAL hMen = GlobalAlloc(GMEM_MOVEABLE, 0);//分配一个可移动的全局内存块（初始0字节）
	if (hMen == NULL)
	{
		//申请内存失败
		return -1;
	}

	//创建一个内存流
	IStream* pStream = NULL;
	HRESULT ret =  CreateStreamOnHGlobal(hMen, true, &pStream);
	if (ret != S_OK) {
		GlobalFree(hMen);          // 创建流失败，流没接管 hMen，自己释放
		return -1;                 // 直接返回（此时也无需再 Release pStream，它是 NULL）
	}
	
	//将文件保留在内存流里面  png格式
	image.Save(pStream, ::Gdiplus::ImageFormatPNG);
	//将流指针放到开头  STREAM_SEEK_SET:开头
	LARGE_INTEGER lg = { 0 };
	pStream->Seek(lg, STREAM_SEEK_SET, NULL);

	//获取流指针
	char* pdata = (char*)GlobalLock(hMen);
	//获取长度
	int len = GlobalSize(hMen);

	// ---- 发送数据（循环发送，处理部分发送）----
	Packet* pck = PackPacket(CMD_SCREEN, pdata, len);
	if (pck == NULL)
	{
		printf("打包失败\r\n");
		GlobalUnlock(hMen);
		pStream->Release();
		image.Destroy();
		return -1;
	}

	int total = sizeof(PacketHeader) + len;   // 整个包的总字节数
	int sent = 0;                             // 已发送字节数
	char* p = (char*)&pck->header.magic;      // 从 magic 开始，即包头第一个字节

	while (sent < total)
	{
		int n = send(g_client_socket, p + sent, total - sent, 0);
		if (n <= 0)
		{
			printf("发送失败:%d\r\n", n);
			break;
		}
		sent += n;
	}

	if (sent == total)
		printf("发送成功:%d\r\n", sent);

	free(pck);
	GlobalUnlock(hMen);
	


	if (pStream) pStream->Release();//释放流对象
	image.Destroy();//释放图像对象的DC

	//通过网络发送
	return 0;
}

int HandleMouse(Packet* packet)
{
	//取出鼠标信息
	Mouse mouse;
	memset(&mouse, 0, sizeof(mouse));   // 先清零，防止 body 不足时读到垃圾坐标
	memcpy(&mouse, packet->body, min(packet->header.body_len, sizeof(Mouse)));
	printf("x = %d, y = %d", mouse.ptXY.x, mouse.ptXY.y);

	// 模拟执行鼠标事件
	//设置鼠标位置
	SetCursorPos(mouse.ptXY.x, mouse.ptXY.y);

	switch (mouse.action)
	{
	case MOUSE_MOVE:
		break;
	case MOUSE_LDOWN:
		mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,GetMessageExtraInfo());
		break;
	case MOUSE_LUP:
		mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_RDOWN:
		mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_RUP:
		mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_MDOWN:
		mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_MUP:
		mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_LCLICK:
		mouse_event(MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_RCLICK:
		mouse_event(MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_MCLICK:
		mouse_event(MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_MIDDLEUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_LDCLICK:
		mouse_event(MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP, 0, 0, 0, GetMessageExtraInfo());
		mouse_event(MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_RDCLICK:
		mouse_event(MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP, 0, 0, 0, GetMessageExtraInfo());
		mouse_event(MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	case MOUSE_MDCLICK:
		mouse_event(MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_MIDDLEUP, 0, 0, 0, GetMessageExtraInfo());
		mouse_event(MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_MIDDLEUP, 0, 0, 0, GetMessageExtraInfo());
		break;
	default:
		printf("未知鼠标操作: %d\r\n",mouse.action);
		break;
	}

	return 0;
}

int HandleKeyBoard(Packet* packet)
{
	KeyBoard key_board;
	if (packet->header.body_len >= (int)sizeof(KeyBoard))
		memcpy(&key_board, packet->body, sizeof(KeyBoard));
	else
		return -1;

	INPUT input = { 0 };
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = key_board.virtual_code;
	input.ki.wScan = 0;
	input.ki.time = 0;
	input.ki.dwFlags = key_board.key_status; // 按钮状态 0-按下 1-松开
	input.ki.dwExtraInfo = 0;
	
	int ret = SendInput(1, &input, sizeof(INPUT));
	if (ret > 0)
	{
		printf("成功执行键盘事件：%d\r\n", key_board.virtual_code);
	}


	return 0;
}

int HandleTestConnect(Packet* packet)
{
	
	return 0;
}

Packet* ParsePacket(char* buffer, int len)
{
	if (len < (int)sizeof(PacketHeader))   // 连头都不够，等下次
		return NULL;

	int index = 0;
	// 安全扫描 magic：保证 index+4 <= len，不越界
	for (; index + 4 <= len; index++)
	{
		if (*(int*)(buffer + index) == 0x12345678)
			break;
	}
	if (index + 4 > len)                   // 没找到 magic
		return NULL;

	int magic = *(int*)(buffer + index);   index += 4;
	if (index + 8 > len)                   // cmd / body_len 还没收全
		return NULL;

	int cmd = *(int*)(buffer + index); index += 4;
	int body_len = *(int*)(buffer + index); index += 4;

	if (body_len < 0)                      // 非法长度
		return NULL;
	if (index + body_len > len)            // 半包，等下次 recv 补齐
		return NULL;

	Packet* ppck = (Packet*)malloc(sizeof(PacketHeader) + body_len);
	if (ppck == NULL)
		return NULL;

	ppck->header.magic = magic;
	ppck->header.cmd = cmd;
	ppck->header.body_len = body_len;
	
	memcpy(ppck->body, buffer + index, body_len);

	return ppck;
}

int GetPacketLen(Packet* pck)
{
	if (pck != NULL)
	{
		return pck->header.body_len + sizeof(PacketHeader);
	}
	return 0;
}


Packet* PackPacket(int cmd, char* buffer, int buffer_len)
{
	Packet* pck = (Packet*)malloc(sizeof(PacketHeader) + buffer_len);
	pck->header.magic = 0x12345678; //包头
	pck->header.cmd = cmd; //命令号
	pck->header.body_len = buffer_len;//数据长度

	if (buffer_len > 0 && buffer != NULL)
	{
		memcpy(pck->body, buffer, buffer_len);
	}

	return pck;
}


int InitServer()
{
	//1.初始化网络服务器
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	//2.创建套接字socket  AF_INET:ipv4协议  SOCK_STREAM:使用TCP协议  SOCK_DGRAM：UDP协议  0:默认协议
	g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (g_server_socket == INVALID_SOCKET)
	{
		printf("创建套接字失败！\n");
		return -1;
	}

	//3.给服务器socket绑定地址
	SOCKADDR_IN server_addr;
	server_addr.sin_family = AF_INET; //协议族 ipv4协议
	server_addr.sin_port = htons(9999); //端口号  0-1024 htons函数将主机字节序转换为网络字节序
	//server_addr.sin_addr.S_un.S_addr = inet_addr(""); //IP地址  0.0.0.0 监听服务器上所有的ip  INADDR_ANY:本地所有IP地址
	server_addr.sin_addr.S_un.S_addr = inet_addr("0.0.0.0"); //IP地址  0.0.0.0 监听服务器上所有的ip  INADDR_ANY:本地所有IP地址
	if (bind(g_server_socket, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
	{
		printf("绑定地址失败！\n");
		return -2;
	}

	//4.开启服务器socket监听     backlog:最大连接数
	if (listen(g_server_socket, 5) == SOCKET_ERROR)//监听套接字  第二个参数：最大连接数
	{
		printf("开启监听失败！\n");
		return -3;
	}

	return 0;
}