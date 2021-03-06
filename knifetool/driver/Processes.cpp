#include "Predef.h"
#include "Processes.h"

#define MM_TAG_PROC		'PROC'

#define EPROCESS_PID_OFFSET					0x84
#define EPROCESS_LINK_OFFSET				0x88
#define EPROCESS_EXITTIME_OFFSET			0x78
#define EPROCESS_OBJTABLE_OFFSET			0xC4
#define EPROCESS_FILENAME_OFFSET			0x174 
#define EPROCESS_PEB_OFFSET					0x1B0
#define PEB_PROCPARAM_OFFSET				0x10
#define PEB_PROCPARAM_IMAGEPATH_OFFSET		0x38


typedef BOOLEAN (__stdcall*EX_ENUMERATE_HANDLE_ROUTINE)(PHANDLE_TABLE_ENTRY HandleTableEntry, HANDLE Handle, PVOID EnumParameter);

extern "C"
NTKERNELAPI
BOOLEAN
ExEnumHandleTable (
				   __in PHANDLE_TABLE                   HandleTable,
				   __in EX_ENUMERATE_HANDLE_ROUTINE     EnumHandleProcedure,
				   __in PVOID     EnumParameter,
				   __out_opt PHANDLE Handle
				   );



BOOLEAN ExEnumHandleCallBack(PHANDLE_TABLE_ENTRY HandleTableEntry, HANDLE Handle, PVOID EnumParameter)
{
    NTSTATUS ntStatus;
    HANDLE Cid;
    PEPROCESS pEproc;
    ULONG uTableCount;
    ULONG uTablePage = 0;
    
    if(EnumParameter == HandleTableEntry)
    {
        return TRUE;
    }
    else
    {
        for(uTableCount = 0; uTableCount < 0x1000; uTableCount++)
        {
            if(HandleTableEntry->Object)
            {
                Cid = (HANDLE)((1024*uTablePage)+(uTableCount<<2));
                if(Cid > (PVOID)4)
                {
                    ntStatus = PsLookupProcessByProcessId(Cid, &pEproc);
                    if(NT_SUCCESS(ntStatus))
                    {
						KdPrint(("PID = %d\tProcess Name = %s", Cid, ((PUCHAR)pEproc+EPROCESS_FILENAME_OFFSET)));                              
                        ObDereferenceObject(pEproc);
                    }
                }
                else
                {
                    if(Cid == 0)
                    {
						KdPrint(("PID = %d/tProcess Name:Idle", 0));                         
                    }
                    else
                    {
						KdPrint(("PID = %d/tProcess Name:System", 4));                      
                    }
                }
            }
        }
        uTablePage++;
        return TRUE;
    }
}

void GetPspCidTable(LPVOID *pPspCidTable)
{
    PUCHAR cPtr;
    DWORD PsAddr = GetFunctionAddr(L"PsLookupProcessByProcessId");
    
    for(cPtr = (PUCHAR)PsAddr;cPtr < (PUCHAR)PsAddr + PAGE_SIZE; cPtr++)
    {
        if(*(PUSHORT)cPtr == 0x35FF)
        {
            *pPspCidTable = **(PVOID**)(cPtr+2);
            break;
        }
    }
}

BOOL GetProcessImagePathByPeb(ULONG Pid, PWCHAR wsImagePath)
{
	BOOL bRet = FALSE;
	ULONG ulEproc, ulPeb, ulProcParam;		
	NTSTATUS status = PsLookupProcessByProcessId((HANDLE)Pid, (PEPROCESS*)&ulEproc);
	if(NT_SUCCESS(status))
	{
		KeAttachProcess((PRKPROCESS)ulEproc);	
		ulPeb = *(ULONG*)(ulEproc + EPROCESS_PEB_OFFSET);
		if(MmIsAddressValid((PVOID)ulPeb))
		{
			ulProcParam  = *(ULONG*)(ulPeb + PEB_PROCPARAM_OFFSET);
			if(MmIsAddressValid((PVOID)ulProcParam))
			{
				PUNICODE_STRING imagePath = (PUNICODE_STRING)(ulProcParam+PEB_PROCPARAM_IMAGEPATH_OFFSET);
				if(imagePath && imagePath->Buffer !=NULL)
				{
					RtlStringCchCopyW(wsImagePath, MAX_PATH, imagePath->Buffer);
					bRet = TRUE;
				}				
			}
		}
		KeDetachProcess();
		ObDereferenceObject((PVOID)ulEproc);			
	}	
	return bRet;
} 


NTSTATUS EnumProcessesByNativeApi(LPVOID pProcessesBuf)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	
	if(pProcessesBuf != NULL)
	{
		ULONG ulSize = 0x8000;		
		LPVOID lpBuf = ExAllocatePoolWithTag(NonPagedPool, ulSize, MM_TAG_PROC);
		while( lpBuf != NULL && 
			(status = ZwQuerySystemInformation(SystemProcessesAndThreadsInformation, lpBuf, ulSize, NULL) == STATUS_INFO_LENGTH_MISMATCH ))
		{
			ExFreePool(lpBuf);
			lpBuf = NULL;
			ulSize*=2;
			lpBuf = ExAllocatePoolWithTag(NonPagedPool, ulSize, MM_TAG_PROC);		
		}	
		
		if(NT_SUCCESS(status))
		{
			PSYSTEM_PROCESS_INFORMATION  pSPI= (PSYSTEM_PROCESS_INFORMATION)lpBuf;
			PPROCESS_INFO_LIST pProcInfoList = (PPROCESS_INFO_LIST)pProcessesBuf;
			ULONG ulIndex = 0;
			while(TRUE)
			{			
				ULONG ulPid =  (ULONG)pSPI->ProcessId;
				pProcInfoList->ProcInfo[ulIndex].ulPID = ulPid;
				if(ulPid == 0)
				{
					RtlStringCchCopyW(pProcInfoList->ProcInfo[ulIndex].ImageName, MAX_PATH, L"System Idle Process");
				}
				else
				{
					if(pSPI->ImageName.Length > 0 && pSPI->ImageName.Buffer != NULL)
					{
						RtlStringCchCopyW(pProcInfoList->ProcInfo[ulIndex].ImageName, MAX_PATH, pSPI->ImageName.Buffer);
					}
				}
				if(ulPid == 0 || ulPid == 4)
				{
					RtlStringCchCopyW(pProcInfoList->ProcInfo[ulIndex].ImagePath, MAX_PATH, L"NT OS Kernel");
				}
				if(!GetProcessImagePathByPeb(ulPid, pProcInfoList->ProcInfo[ulIndex].ImagePath) && ulPid > 4)
				{
					UNICODE_STRING ustrImgPath;
					GetProcessImagePath(ulPid, &ustrImgPath);
					if(ustrImgPath.Length > 0 && ustrImgPath.Buffer != NULL)
					{
						RtlStringCchCopyW(pProcInfoList->ProcInfo[ulIndex].ImagePath, MAX_PATH, ustrImgPath.Buffer);
					}
				}			
				pProcInfoList->ProcInfo[ulIndex].ulMemory = pSPI->VirtualMemoryCounters.PeakWorkingSetSize;
				pProcInfoList->ProcInfo[ulIndex].Priority = pSPI->BasePriority;
				pProcInfoList->ProcInfo[ulIndex].ulThreads = pSPI->NumberOfThreads;				
				ulIndex++;				
				
				if (pSPI->NextEntryOffset == 0) 
					break;
				
				pSPI = (PSYSTEM_PROCESS_INFORMATION)(((PUCHAR)pSPI)+ pSPI->NextEntryOffset); 
			}
			
			pProcInfoList->ulCount = ulIndex;
		}
		if(lpBuf)
			ExFreePool(lpBuf);		
	}
	
	return status;	
}

BOOL CheckProcess(ULONG ulEprocess, ULONG ulsysEprocess)
{
	ULONG ulStartAddr;
	__asm
	{
		mov eax, MmSystemRangeStart
		mov eax, [eax]
		mov ulStartAddr, eax
	}	


	if(ulEprocess > ulsysEprocess || ulEprocess < ulStartAddr)
	{
		return FALSE;
	}
	
	if (*(ULONG *)( ulEprocess + EPROCESS_EXITTIME_OFFSET) != 0)
	{		
		return FALSE;		
	}

	if (*(ULONG *)(ulEprocess + EPROCESS_OBJTABLE_OFFSET) == 0)
	{
		return FALSE;
	}

	return TRUE;	
}

//no idle process
NTSTATUS EnumProcessesByActiveList()
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	ULONG ulSysEproc = (ULONG)PsGetCurrentProcess();
	LIST_ENTRY*  pActiveLinks;
	if(ulSysEproc != 0)
	{	
		ULONG ulActiveProc = ulSysEproc;
		do
		{
			KdPrint(("EPROCESS= 0x%08X", ulActiveProc));
			if( CheckProcess(ulActiveProc, ulSysEproc) )
			{
				ULONG pID = *(ULONG *)(ulActiveProc + EPROCESS_PID_OFFSET);
				KdPrint(("Process Id = %d", pID));
			}

			pActiveLinks = (LIST_ENTRY *)(ulActiveProc + EPROCESS_LINK_OFFSET); 
		
			ulActiveProc = (ULONG)pActiveLinks->Flink-EPROCESS_LINK_OFFSET;

			if(ulActiveProc == ulSysEproc)
			{
				status = STATUS_SUCCESS;
				break;
			}
		}while(ulActiveProc);
	}

	return status;
}


NTSTATUS EnumProcessesByCidTable()
{	
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	LPVOID pPspCidTable = NULL;
    GetPspCidTable(&pPspCidTable);
	KdPrint(("CidTable = 0x%08X", pPspCidTable));
	if(pPspCidTable != NULL)
	{
		HANDLE handle;    
		if(ExEnumHandleTable((PHANDLE_TABLE)pPspCidTable, ExEnumHandleCallBack, NULL, &handle))
		{
			status = STATUS_SUCCESS;
		}
	}
    return status;
}

//这个方式获取路径可即时更新
//BOOL GetProcessImagePath()
//{	
//	//eprocess ¡ú SectionObject(_SECTION_OBJECT)-> _SEGMENT_OBJECT(MS ·ûºÅ¿â¿ÉÄÜÓÐÎÊÌâ£¬Êµ¼Ê²âÊÔÊÇ_SEGMENT) ¡ú_CONTROL_AREA¡ú_FILE_OBJECT
//	BOOL bRet = FALSE;
//	ULONG ulEproc, ulSectionObject, ulSegment, ulControlArea;		
//	NTSTATUS status = PsLookupProcessByProcessId((HANDLE)1272, (PEPROCESS*)&ulEproc);
//	if(NT_SUCCESS(status))
//	{
//		ulSectionObject = *(ULONG*)(ulEproc + 0x138 );
//		if(MmIsAddressValid((PVOID)ulSectionObject))
//		{			
//			ulSegment  = *(ULONG*)(ulSectionObject + 0x014 );
//			if(MmIsAddressValid((PVOID)ulSegment))
//			{
//				ulControlArea  = *(ULONG*)(ulSegment);
//				if(MmIsAddressValid((PVOID)ulControlArea))
//				{
//					PFILE_OBJECT  fileObj = *(PFILE_OBJECT *)(ulControlArea+0x024 );
//					if(fileObj !=NULL)
//					{
//						UNICODE_STRING filepath ;//= {0};
//						//	filepath.Length = 0;
//						//	filepath.MaximumLength = 256*sizeof(WCHAR);
//						//	filepath.Buffer =(WCHAR*) ExAllocatePoolWithTag( NonPagedPool, 256*sizeof(WCHAR), 'tex');
//
//						//RtlVolumeDeviceToDosName only for win2K, xp crash
//						IoVolumeDeviceToDosName(fileObj->DeviceObject,&filepath );
//						KdPrint(("Image path = %ws", filepath.Buffer));	
//						KdPrint(("Image path = %wZ", &filepath));	
//						ExFreePool(filepath.Buffer);
//
//
//						//不要构造，构造时回自己分配内存，覆盖站空间。导致crash
//						ULONG ReturnLength;
//						UCHAR Buffer[sizeof(OBJECT_NAME_INFORMATION)+(256*sizeof(WCHAR))];
//						POBJECT_NAME_INFORMATION ObjectNameInfo = (POBJECT_NAME_INFORMATION)Buffer;
//
//						ObQueryNameString(fileObj, ObjectNameInfo, sizeof(Buffer), 
//							&ReturnLength);					
//
//						KdPrint(("ObQueryNameString = %ws, retLen = %d", ObjectNameInfo->Name.Buffer, ReturnLength));					
//
//						bRet = TRUE;
//					}				
//				}
//			}		
//		}
//		ObDereferenceObject((PVOID)ulEproc);
//	}	
//	return bRet;
//} 


//正在使用的程序，在统一盘符下切换不会自己更新
NTSTATUS GetProcessImagePath(IN ULONG Pid, OUT PUNICODE_STRING ProcessImagePath)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	OBJECT_ATTRIBUTES ObjAttrs;
	CLIENT_ID clientid;
	InitializeObjectAttributes(&ObjAttrs, 0 ,OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, 0, 0);
	clientid.UniqueProcess = (HANDLE)Pid;
	clientid.UniqueThread=0;
	HANDLE hProc = NULL;
	status = ZwOpenProcess(&hProc, PROCESS_ALL_ACCESS, &ObjAttrs, &clientid); 
	if (NT_SUCCESS(status)) 
	{
		ULONG ulRetSize = 0;
		status = ZwQueryInformationProcess(hProc, ProcessImageFileName, NULL, 0, &ulRetSize);
		if(status == STATUS_INFO_LENGTH_MISMATCH && ulRetSize > 0)
		{
			LPVOID lpBuf = ExAllocatePoolWithTag(NonPagedPool, ulRetSize, MM_TAG_PROC);
			if(lpBuf != NULL)
			{
				status=ZwQueryInformationProcess(hProc, ProcessImageFileName, lpBuf, ulRetSize, &ulRetSize);
				if (NT_SUCCESS(status)) 
				{
					PUNICODE_STRING  pImagePath = (PUNICODE_STRING)lpBuf;				
					RtlInitUnicodeString(ProcessImagePath, pImagePath->Buffer);
				}
				ExFreePool(lpBuf);	
			}
		}
	}	
	return status;
}

NTSTATUS HideProcessByActiveList(IN ULONG Pid)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	PEPROCESS pEProc = NULL;
	status = PsLookupProcessByProcessId((HANDLE)Pid, &pEProc);
	if(NT_SUCCESS(status))
	{
		ULONG ulSysEproc = (ULONG)PsGetCurrentProcess();
		LIST_ENTRY*  pActiveLinks;
		if(ulSysEproc != 0)
		{	
			ULONG ulActiveProc = ulSysEproc;
			do
			{			
				ULONG ulActivePid = *(ULONG *)(ulActiveProc + EPROCESS_PID_OFFSET);				
				pActiveLinks = (LIST_ENTRY *)(ulActiveProc + EPROCESS_LINK_OFFSET); 

				if(ulActivePid == Pid)
				{
					RemoveEntryList(pActiveLinks);				
					status = STATUS_SUCCESS;
					break;
				}
				
				ulActiveProc = (ULONG)pActiveLinks->Flink-EPROCESS_LINK_OFFSET;				
				
				if(ulActiveProc == ulSysEproc)
				{				
					break;
				}

			}while(ulActiveProc);
		}	
	}
	return status;

}

//
//NTSTATUS ProtectProcessByPidUseThreadFlags(ULONG pid) 
//{ 
//	ULONG pThNextEntry,pThListHead,pProtectProcess,pTempThread; 
//	PEPROCESS EProcess; 
//	NTSTATUS status; 
//
//	status = PsLookupProcessByProcessId((HANDLE)pid,&EProcess); 
//	if((NT_SUCCESS(status))) 
//	{ 
//		pProtectProcess=(ULONG)EProcess; 
//		pThListHead = pProtectProcess+THREAD_LIST_HEAD_OFFSET; //0x50
//		pThNextEntry=*(ULONG *)pThListHead; 
//		while(pThNextEntry != pThListHead) 
//		{ 
//			pTempThread =pThNextEntry-THREAD_LIST_ENTRY_OFFSET;  // 0x1b0 	
//
//
//
//			//	*(ULONG*)(pTempThread+CrossThreadFlags_OFFSET) ^= PS_CROSS_THREAD_FLAGS_SYSTEM;	
//
//			*(ULONG*)(pTempThread+CrossThreadFlags_OFFSET) ^= PS_CROSS_THREAD_FLAGS_TERMINATED;	
//
//			pThNextEntry = *(ULONG *)pThNextEntry; 
//
//		} 
//	} 
//	return status; 
//} 
