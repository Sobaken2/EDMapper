#include "edmapper.hpp"


int main()
{
	// get process id

	g_process_id = Edmapper::GetProcessID("notepad.exe");


	if (g_process_id == 0) {
		std::printf("[-]Couldn't get process ID\n");
		return -1;
	}


	// open handle to process

	gProc_handle = Edmapper::OpenProcessHandle(g_process_id);

	// add check for handle here.

	// read dll

	if (Edmapper::GetRawDataFromFile("C:\\Users\\User\\Desktop\\cpp-projects\\EDMapper\\x64\\Release\\test.dll") != false)
	{
		std::cout << "read dll." << '\n';
	}

	// validate image

	if (Edmapper::IsValidImage())
	{
		std::cout << "Image is valid." << '\n';
	}

	// allocate local image 
	const auto image_size = pOldnt_headers->OptionalHeader.SizeOfImage;
	void* l_image = nullptr;
	l_image = VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	
	if (!l_image)
	{
		delete[] rawDll_data;
		return -1;
	}

	// copy all headers from our dll image.
	std::memcpy(l_image, rawDll_data,pOldnt_headers->OptionalHeader.SizeOfHeaders);


	// copy sections into local image.
	Edmapper::CopyImageSections(l_image,pOldnt_headers);


	// fix imports
	if (!Edmapper::FixImageImports(l_image, pOldnt_headers))
	{
		std::cerr << "[ERROR] couldn't fix image imports" << '\n';
		delete[] rawDll_data;
		VirtualFree(l_image, 0, MEM_RELEASE);
		return -1;
	}

	std::printf("Imports fixed.\n");
		

	// allocate image in target process
	const auto image_base = pOldnt_headers->OptionalHeader.ImageBase;
	void* m_image = nullptr;
	// first try to allocate at prefered load address
	m_image = VirtualAllocEx(gProc_handle.get(), reinterpret_cast<LPVOID>(image_base), image_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

	if (!m_image)
	{
		// if we couldn't allocate at prefered address then just allocate memory on any random place in memory 
		// but we will need to fix relocation of image
		m_image = VirtualAllocEx(gProc_handle.get(), nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

		// if faild to allocate then cleanup & return
		if (!m_image)
		{
			delete[] rawDll_data;
			VirtualFree(l_image, 0, MEM_RELEASE);
			std::cerr << "[ERROR] couldn't allocate memory in target process." << '\n';
			return -1;
		}

		// fix relocation
		Edmapper::FixImageRelocations(m_image,l_image, pOldnt_headers);

		std::printf("Fixed relocations!\n");
	}

	// no need to fix relocations since we loaded at prefered base address.

	
	// write content of our dll aka local image into the allocated memory in target process
	if (!Edmapper::Write(reinterpret_cast<std::uintptr_t>(m_image),l_image, image_size))
	{
		delete[] rawDll_data;
		VirtualFree(l_image, 0, MEM_RELEASE);
		VirtualFreeEx(gProc_handle.get(),m_image, 0, MEM_RELEASE);
		std::cerr << "[ERROR] couldn't copy image to target process." << '\n';
		return -1;
	}

	std::printf("Wrote image to target process.\n");

	

	// call shellcode

	// get entrypoint address for our dll
	// pOldnt_headers->OptionalHeader.AddressOfEntryPoint : is an RVA from base address
	const auto Entryaddress = reinterpret_cast<std::uintptr_t>(m_image) + pOldnt_headers->OptionalHeader.AddressOfEntryPoint;

	/*
	BYTE shellcode[] =
	{
	    0x50, // push rax save old register so we don't corrupt it
		0x48, 0xB8, 0xFF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, // mov rax,0xff00efbeadde00ff <- this value is just a place that will get replaced by our entrypoint pointer 
		0x48, 0x83, 0xEC, 0x28, // sub rsp,0x28 (align the stack and shadow space allocation)
		0xFF, 0xD0, // call rax 
		0x48, 0x83, 0xC4, 0x28, // add rsp,0x28
		0x58, // pop rax
		0xC3 // ret
	};*/


	// copy address to shellcode
	// *(std::uintptr_t*)(shellcode + 3) = Entryaddress;

	// hardcoded shellcode
	BYTE shellcode[] = {
		0x50, // push rax
		0x48, 0xB8, 0xFF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, // mov rax,address
		0x48, 0x31, 0xD2, // xor rdx,rdx
		0x48, 0x83, 0xC2, 0x01, //  add rdx,byte +0x0
		0x48, 0x83, 0xEC, 0x28,
		0xFF, 0xD0, // call rax 
		0x48, 0x83, 0xC4, 0x28, // add rsp,0x28
		0x58, // pop rax
		0xC3
	};

	*(std::uintptr_t*)(shellcode + 3) = 0x180001020; // Hardcoded offset

	// allocate memory for our shellcode inside target process
	auto pShellCode = VirtualAllocEx(gProc_handle.get(), nullptr, sizeof(shellcode), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!pShellCode)
	{
		delete[] rawDll_data;
		VirtualFreeEx(gProc_handle.get(), m_image, 0, MEM_RELEASE);
		VirtualFree(l_image, 0, MEM_RELEASE);
		std::cerr << "[ERROR] failed to allocate memory for shellcode in target process." << '\n';
		return -1;
	}
	
	std::printf("[+]Allocated memory for shellcode at : %p \n",pShellCode);


	if (!Edmapper::Write((std::uintptr_t)pShellCode,shellcode,sizeof(shellcode)))
	{
		delete[] rawDll_data;
		VirtualFreeEx(gProc_handle.get(), m_image, 0, MEM_RELEASE);
		VirtualFreeEx(gProc_handle.get(), pShellCode, 0, MEM_RELEASE);
		VirtualFree(l_image, 0, MEM_RELEASE);
		std::cerr << "[ERROR] failed to write shellcode to memory" << '\n';
		return -1;
	}


	// this will execute our shellcode inside the target process
	auto thread_h = CreateRemoteThread(gProc_handle.get(), 0, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(pShellCode), 0, 0, 0);
	if (!thread_h)
	{
		delete[] rawDll_data;
		VirtualFreeEx(gProc_handle.get(), m_image, 0, MEM_RELEASE);
		VirtualFreeEx(gProc_handle.get(), pShellCode, 0, MEM_RELEASE);
		VirtualFree(l_image, 0, MEM_RELEASE);
		std::cerr << "[ERROR] failed to create thread in target process." << '\n';
		return -1;
	}

	// wait for the thread to finish executing shellcode.
	WaitForSingleObject(thread_h, INFINITE);


	std::printf("[+] DLL mapped.\n");
	CloseHandle(thread_h);
	// free mapped image?? why lol iwant to understand this
	// when we inject and use MessageBoxA the target process should have it too in its import directory. else we will crash.
	//VirtualFreeEx(gProc_handle.get(), m_image, 0, MEM_RELEASE);
	VirtualFreeEx(gProc_handle.get(), pShellCode, 0, MEM_RELEASE);
	VirtualFree(l_image, 0, MEM_RELEASE);
	delete[] rawDll_data;

	std::cin.get();
}