#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include<stdio.h>
#include<Windows.h>
#include<atlimage.h>


#pragma comment(lib,"ws2_32.lib") //链接库

#define RECV_BUFFER_SIZE 1024 * 1024 * 10

//将这个结构体按照1字节对齐  这样就不会有内存对齐的填充字节  结构体的大小就是成员变量的大小之和
#pragma pack(push,1)
struct PacketHeader
{
	int magic; //4字节包头
	int cmd;   //4字节命令号
	int body_len; //数据长度
};
#pragma pack(pop)

struct Packet
{
	PacketHeader header; //包头
	char body[]; //包体
};
//方便定义整数常量
enum CMD
{
	CMD_SCREEN = 1,
	CMD_MOUSE = 2,
	CMD_TESTCONNECT = 200,
	CMD_KEYBOARD = 4
};

//鼠标信息  按键：右键、左键、中间   状态：按下、放开、双击
enum ENUM_MOUSE {
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



Packet* PackPacket(int cmd,char* buffer, int buffer_len);
Packet* ParsePacket(char* buffer, int len);
int InitSocket();
int InitWindow(HINSTANCE hInstance, int nCmdShow);
int GetPacketLen(Packet* pck);
DWORD WINAPI SendScreenCallBack(LPVOID lpThreadParameter);

SOCKET g_server_socket;
SOCKADDR_IN g_server_addr;
HWND g_hwnd = NULL;
CImage g_image;
int g_remote_width = -1;
int g_remote_height = -1;


CRITICAL_SECTION g_cri_sec;


LRESULT CALLBACK winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static ULONGLONG mouse_tick = GetTickCount64();//获取系统的启动时间（ms）
	switch (msg)
	{
	case WM_PAINT://绘制事件
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		//拿到这个image
		if (!g_image.IsNull())
		{
			//做一个缩放
			//拿到窗口客户区的大小
			RECT client_rect;
			GetClientRect(hwnd, &client_rect);
			int client_width = client_rect.right - client_rect.left;
			int client_height = client_rect.bottom - client_rect.top;


			//设置拉升模式
			//HALFTONE:高质量的拉伸模式  COLORONCOLOR:快速的拉伸模式
			int oldMode = SetStretchBltMode(hdc,HALFTONE);
			//设置画刷原点  选择了高质量拉伸模式后必须设置原点
			SetBrushOrgEx(hdc, 0, 0, NULL);

			EnterCriticalSection(&g_cri_sec);

			//远程图片的宽高
			int remote_width = g_image.GetWidth();
			int remote_height = g_image.GetHeight();
			g_image.StretchBlt(hdc,0,0,client_width,client_height,0,0,remote_width,remote_height,SRCCOPY);

			LeaveCriticalSection(&g_cri_sec);

			//恢复默认的拉伸模式
			SetStretchBltMode(hdc, oldMode);

		}
		EndPaint(hwnd, &ps);
	}
		break;

	case WM_MOUSEMOVE://鼠标移动  一秒发一次请求
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd,&client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_MOVE;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
			mouse_tick = GetTickCount64();
			/*
			if (GetTickCount64() - mouse_tick > 500)
			{
				Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
				send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
				free(packet);
				mouse_tick = GetTickCount64();
			}*/

		}

	}
		break;

	case WM_LBUTTONDOWN://鼠标左键按下
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_LDOWN;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_LBUTTONUP://鼠标左键抬起
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_LUP;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_LBUTTONDBLCLK://鼠标左键双击
	{
		{
			//拿到了当前鼠标位置
			int xPos = LOWORD(lParam); //低两个字节是x坐标
			int yPos = HIWORD(lParam); //高两个字节是y坐标
			RECT client_rect;
			GetClientRect(hwnd, &client_rect);
			int client_width = client_rect.right - client_rect.left;
			int client_height = client_rect.bottom - client_rect.top;
			if (g_remote_width != -1 && g_remote_height != -1)
			{
				int rxPos = xPos * g_remote_width / client_width;
				int ryPos = yPos * g_remote_height / client_height;
				//发送数据
				Mouse mouse;
				mouse.action = MOUSE_LDCLICK;
				mouse.ptXY.x = rxPos;
				mouse.ptXY.y = ryPos;
				Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
				send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
				free(packet);
			}

		}
	}
		break;

	case WM_RBUTTONDOWN://鼠标右键按下
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_RDOWN;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_RBUTTONUP://鼠标右键抬起
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_RUP;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_RBUTTONDBLCLK://鼠标右键双击
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_RDCLICK;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_MBUTTONDOWN://鼠标中键按下
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_MDOWN;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_MBUTTONUP://鼠标中键抬起
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_MUP;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	case WM_MBUTTONDBLCLK://鼠标中键双击
	{
		//拿到了当前鼠标位置
		int xPos = LOWORD(lParam); //低两个字节是x坐标
		int yPos = HIWORD(lParam); //高两个字节是y坐标
		RECT client_rect;
		GetClientRect(hwnd, &client_rect);
		int client_width = client_rect.right - client_rect.left;
		int client_height = client_rect.bottom - client_rect.top;
		if (g_remote_width != -1 && g_remote_height != -1)
		{
			int rxPos = xPos * g_remote_width / client_width;
			int ryPos = yPos * g_remote_height / client_height;
			//发送数据
			Mouse mouse;
			mouse.action = MOUSE_MDCLICK;
			mouse.ptXY.x = rxPos;
			mouse.ptXY.y = ryPos;
			Packet* packet = PackPacket(CMD_MOUSE, (char*)&mouse.action, sizeof(Mouse));
			send(g_server_socket, (char*)&packet->header.magic, GetPacketLen(packet), 0);
			free(packet);
		}

	}
		break;

	//键盘事件
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		//拿到虚拟键码
		//发送数据
		KeyBoard key_board;
		key_board.virtual_code = wParam;
		key_board.key_status = 0;
		Packet* packet = PackPacket(CMD_KEYBOARD, (char*) & key_board.virtual_code, sizeof(KeyBoard));
		send(g_server_socket, (char*)& packet->header.magic, GetPacketLen(packet), 0);
		free(packet);
	}
		break;


	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
		break;
	}

	return 0;
}

//返回值 调用约定 函数名(参数列表){函数体}
DWORD WINAPI SendScreenCallBack(LPVOID lpThreadParameter)
{
	char* recv_buffer = (char*)malloc(RECV_BUFFER_SIZE);
	//不停的发送屏幕数据和解析数据
	while (true)
	{
		Packet* pck = PackPacket(CMD_SCREEN, NULL, 0);
		//发送获取屏幕请求
		int sen_len = send(g_server_socket, (char*)&pck->header.magic, GetPacketLen(pck),0);
		if (sen_len > 0)
		{
			OutputDebugString("发送请求屏幕数据成功！\n");

		}
		else
		{
			OutputDebugString("发送请求屏幕数据失败！\n");
		}
		free(pck);

		//等待接收数据
		int len = recv(g_server_socket,recv_buffer, RECV_BUFFER_SIZE,0);
		if (len > 0)
		{
			Packet* recv_pck = ParsePacket(recv_buffer, len);
			if (recv_pck != NULL)
			{//解析成功

				//处理数据 拿到了屏幕数据
				//绘制屏幕数据
				HGLOBAL hMen =  GlobalAlloc(GMEM_MOVEABLE, 0);
				if (hMen == NULL)  continue;

				IStream* pStream = NULL;
				HRESULT ret = CreateStreamOnHGlobal(hMen, true, &pStream);
				if (ret == S_OK)
				{
					ULONG length = 0;
					pStream->Write(recv_pck->body, recv_pck->header.body_len, &length);
					free(recv_pck);


					//将流指针放到开头  STREAM_SEEK_SET:开头
					LARGE_INTEGER lg = { 0 };
					pStream->Seek(lg, STREAM_SEEK_SET, NULL);

					EnterCriticalSection(&g_cri_sec);
					//将数据移到缓存里面去
					if (!g_image.IsNull())
					{
						g_image.Destroy();
					}
					g_image.Load(pStream);
					if (g_remote_width == -1 && g_remote_height == -1)
					{
						g_remote_width = g_image.GetWidth();
						g_remote_height = g_image.GetHeight();
					}

					LeaveCriticalSection(&g_cri_sec);


					//更新图片——将图片画到窗口上，所有和UI相关的操作不能在非UI流水线（线程）操作
					//通知UI线程重绘窗口
					InvalidateRect(g_hwnd, NULL, FALSE);
					UpdateWindow(g_hwnd);

				}
			}
		}


	}
}


int WINAPI WinMain(
	HINSTANCE hInstance,//当前实例句柄
	HINSTANCE hPrevInstance,//上一个实例句柄  在Win32中这个参数总是为NULL
	PSTR     pCmdLine,//命令行参数 以字符串形式传递给程序
	int      nCmdShow//显示窗口的方式  0:隐藏窗口 1:正常显示 2:最小化 3:最大化
)
{

	//初始化关键代码
	InitializeCriticalSection(&g_cri_sec);

	InitWindow(hInstance, nCmdShow);


	//连接服务器
	InitSocket();
	if (connect(g_server_socket, (SOCKADDR*)&g_server_addr, sizeof(g_server_addr)) == SOCKET_ERROR)
	{
		printf("连接服务器失败！\n");
		return -1;
	}
	OutputDebugString("连接服务器成功！\n");
	//发送命令
	//开一条流水线发送请求屏幕的数据以及解析
	unsigned long send_screen_thread_id = 0;
	HANDLE handle_send_screen = CreateThread(NULL,0,&SendScreenCallBack,NULL,0,&send_screen_thread_id);





	MSG msg = {0};
	//创建消息循环队列
	while (GetMessage(&msg,NULL,0,0))
	{
		TranslateMessage(&msg);//翻译消息
		DispatchMessage(&msg);//分发消息
	}

	return 0;

}

int InitWindow(HINSTANCE hInstance, int nCmdShow)
{
	//1.注册窗口类
	WNDCLASS ws = { 0 };
	LPCSTR CLASS_NAME = "MainWindow";
	ws.lpfnWndProc = winProc;//消息处理函数
	ws.hInstance = hInstance;//当前实例句柄
	ws.lpszClassName = CLASS_NAME;

	ws.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);//窗口背景颜色
	ws.hCursor = LoadCursor(NULL, IDC_ARROW);//鼠标指针
	ws.hIcon = LoadIconA(NULL, IDI_APPLICATION);//窗口图标
	ws.style = CS_HREDRAW | CS_VREDRAW;//窗口样式  当窗口水平或垂直调整大小时重绘窗口

	if (!RegisterClass(&ws))
	{
		MessageBoxA(NULL, "注册窗口类失败！", "错误", MB_OK | MB_ICONERROR);
		return 0;
	}

	//2.创建窗口
	g_hwnd = CreateWindow(
		CLASS_NAME, //窗口类名
		"远程控制",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		800, 600,//窗口宽高
		NULL,//父窗口的句柄
		NULL,//菜单句柄
		hInstance,//当前实例句柄
		NULL  //额外参数
	);

	if (g_hwnd == NULL)
	{
		MessageBoxA(NULL, "创建窗口失败！", "错误", MB_OK | MB_ICONERROR);
		return 0;
	}

	//3.显示窗口
	ShowWindow(g_hwnd, nCmdShow);

	//更新窗口
	UpdateWindow(g_hwnd);
}


/*main函数是控制台的入口
int main()
{




	getchar();
	//2.连接服务器
	if (connect(g_server_socket, (SOCKADDR*)&g_server_addr, sizeof(g_server_addr)) == SOCKET_ERROR)
	{
		printf("连接服务器失败！\n");
		return -1;
	}
	printf("连接服务器成功！\n");

	//3.发送数据给服务器
	char buffer[1024] = "Hello, Server!";
	char* recv_buffer  = (char*) malloc(RECV_BUFFER_SIZE);

	int count = 0;

	while (true)
	{
		count++;
		//sprintf_s(buffer, "packet: %d", count);
		//sprintf(buffer, "packet: %d", count);
		printf("请输入要发送的数据：");
		fgets(buffer, 1024, stdin);//接收用户输入


		Packet* send_packet = PackPacket(2000, buffer, 10);
		send(g_server_socket, (char*)&send_packet->header.magic, send_packet->header.body_len + sizeof(PacketHeader), 0); //发送数据给服务器
		free(send_packet);


		printf("发送数据给服务器：%s\n", buffer);

		//4.等待服务器发送数据
		int len = recv(g_server_socket, recv_buffer, RECV_BUFFER_SIZE, 0);
		if (len > 0)
		{
			Packet* recv_pck = ParsePacket(recv_buffer, len);
			printf("收到服务端数据packet->body：%s\n", recv_pck->body);
			printf("收到服务端数据packet->header.magic：%x\n", recv_pck->header.magic);
			printf("收到服务端数据packet->header.cmd：%d\n", recv_pck->header.cmd);
			printf("收到服务端数据packet->header.body_len：%d\n", recv_pck->header.body_len);
			if (recv_pck->header.cmd == 1)
			{
				//服务器返回了屏幕数据
				//解析显示数据

			}

			free(recv_pck);
		}
	}


	closesocket(g_server_socket); //关闭socket
	WSACleanup();

	return 0;

}
*/

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


Packet* ParsePacket(char* buffer, int len)
{
	Packet pck;
	Packet* ppck;
	int index = 0;
	for (; index < len; index++)
	{
		//找包头
		if (*(int*)(buffer + index) == 0x12345678)
		{
			pck.header.magic = *(int*)(buffer + index);
			index += 4;
			break;
		}
	}

	pck.header.cmd = *(int*)(buffer + index);
	index += 4;

	pck.header.body_len = *(int*)(buffer + index);
	index += 4;

	//获取数据
	if (pck.header.body_len > 0)
	{
		//创建接受缓冲区
		ppck = (Packet*)malloc(sizeof(PacketHeader) + pck.header.body_len);

		//拷贝用户数据
		memcpy(ppck->body, buffer + index, pck.header.body_len);

		//拷贝包头
		memcpy(&ppck->header, &pck.header, sizeof(PacketHeader));

		return ppck;
	}
}

int InitSocket()
{
	//初始化网络服务器
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	//客户端网络编程步骤
	//1.创建socket
	g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (g_server_socket == INVALID_SOCKET)
	{
		printf("创建套接字失败！\n");
		return -1;
	}

	g_server_addr.sin_family = AF_INET;
	g_server_addr.sin_port = ntohs(9999); //转换为网络字节序

	g_server_addr.sin_addr.S_un.S_addr = inet_addr("192.168.150.129");
}


int GetPacketLen(Packet* pck)
{
	if (pck != NULL)
	{
		return pck->header.body_len + sizeof(PacketHeader);
	}
	return 0;
}
