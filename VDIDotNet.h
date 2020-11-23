#pragma once
#define _WIN32_DCOM
#include "vdi.h"
#include "vdierror.h"
#include "vdiguid.h"
#include "vcclr.h"
#include <msclr/marshal.h>

using namespace System::Data;
using namespace System::Data::SqlClient;
using namespace System::Runtime::InteropServices;
using namespace System::IO;
using namespace System::Threading;
using namespace System::Threading::Tasks;
using namespace System;

//#define xDEBUG 1 //remove for production

//using namespace Alphaleonis::Win32;
//using namespace System::Collections::Generic;

namespace VDIDotNet
{
	///Class to handle binding a Func^lt;argument,return> to a Func&lt;return>
	///Useful for things like Task::Run(), which does not support a signature with generic arguments
	template <typename T, typename TResult>
	ref class FuncBind
	{
	public:
		delegate TResult _delegate(T);

		FuncBind(_delegate ^func, T args) : m_funcWithArgs(func), m_args(args) {};
		Func<TResult> ^operator()() { return gcnew Func<TResult>(this, &FuncBind::m_funcNoArgs); };

	private:
		_delegate ^m_funcWithArgs;
		T m_args;

		TResult m_funcNoArgs()
		{
			return m_funcWithArgs(m_args);
		};
	};

	///Class to handle binding an Action^lt;argument,return> to a Action&lt;return>
	///Useful for things like Task::ContinueWith(), which does not support a signature with generic arguments
	template <typename T, typename ARG1>
	ref class ActionBind
	{
	public:
		delegate Void _delegate(T);

		ActionBind(_delegate^ func, T args) : m_funcWithArgs(func), m_args(args) {};
		Action<ARG1>^ operator()() { return gcnew Action<ARG1>(this, &ActionBind::m_funcOneArg); };

	private:
		_delegate^ m_funcWithArgs;
		T m_args;

		Void m_funcOneArg(ARG1 arg)
		{
			return m_funcWithArgs(m_args);
		};
	};

	public ref class VDIEngine : IDisposable
	{
	public:
		///Constructor, use format of servername\instancename (omit instance to use default)
		VDIEngine(String ^SQLServer, String ^databaseName, int VDITimeoutMilliseconds)
		{
			if (String::IsNullOrEmpty(SQLServer)) SQLServer = "locahost";
			this->SQLServer = SQLServer;
			auto split = SQLServer->Split('\\');
			if (split->Length > 1) this->instanceName = split[1];
			if (!String::IsNullOrEmpty(databaseName)) this->databaseName = databaseName->Replace(";", ""); else this->databaseName = "";
			this->VDITimeout = VDITimeoutMilliseconds;
			this->Initialize();
		}

		~VDIEngine()
		{
			this->!VDIEngine();
		}

		!VDIEngine()
		{
			//VDILog("VDI Dispose, stream is null=" + (this->VDIStream != nullptr ? "true" : "false") + ", disposeaftertask=" + (this->DisposeVDIStreamAfterReadWriteTask ? "true":"false"));

			this->AbortStream();
			this->DBExecuteCancel();

			if (this->DisposeVDIStreamAfterReadWriteTask && this->VDIStream != nullptr)
			{
				if (this->WriteTask != nullptr)
				{
#if xDEBUG > 1
					VDILog("Queue Write Task Dispose");
#endif
					//create Func delegate for Task ContinueWith
					auto d_ContinueWith = gcnew ActionBind<Stream^, Task^>::_delegate(VDIEngine::CloseVDIStream);
					//wrap Action<Stream, Task> in ActionBind, which in turn exposes an Action<Task> that can be passed to Task::ContinueWith()
					auto b_ContinueWith = gcnew ActionBind<Stream^, Task^>(d_ContinueWith, this->VDIStream);
					this->WriteTask->ContinueWith(b_ContinueWith());
				}
				else if (this->ReadTask != nullptr)
				{
#if xDEBUG > 1
					VDILog("Queue Read Task Dispose");
#endif
					//create Func delegate for Task ContinueWith
					auto d_ContinueWith = gcnew ActionBind<Stream^, Task^>::_delegate(VDIEngine::CloseVDIStream);
					//wrap Action<Stream, Task> in ActionBind, which in turn exposes an Action<Task> that can be passed to Task::ContinueWith()
					auto b_ContinueWith = gcnew ActionBind<Stream^, Task^>(d_ContinueWith, this->VDIStream);
					this->ReadTask->ContinueWith(b_ContinueWith());
				}
				else
				{
#if xDEBUG > 1
					VDILog("Manually Close VDI Stream");
#endif
					CloseVDIStream(this->VDIStream);
				}
			}

			if (this->vds != NULL)
			{
				this->vds->Close();
				this->vds->Release();
				this->vds = NULL;
			}
		}

		//event EventHandler<SqlInfoMessageEventArgs ^> ^InfoMessage;

		String ^VDIDeviceName = nullptr;
		Thread ^DBThread = nullptr;

		int VDITimeout;

		volatile bool DBBackupRunning = false;
		volatile bool DBThreadRunning = false;
		volatile bool DBStreamReady = false;
		Int32 PercentComplete = 0;
		volatile bool StreamComplete = false; //for backups, this is set to true after all bytes have been streamed, but we are still waiting for confirmation from the far end
		Int64 TotalBytesTransferred = 0;
		volatile Exception ^DBError = nullptr;
		//String ^DBErrorMessage = "";

		Task<int> ^ReadTask = nullptr;
		Task ^WriteTask = nullptr;

		Stream^ VDIStream = nullptr; //holds stream used in ProcessVDIStream, so it can be disposed after the read/write task finishes.
		volatile bool DisposeVDIStreamAfterReadWriteTask = false;

	private:
		volatile bool StreamRunning = false;
		volatile bool StreamAbort = false;

		static Void CloseVDIStream(Stream ^stream)
		{
#if xDEBUG > 1
			VDILog("CloseVDIStream() called");
#endif
			if (stream != nullptr)
			{
				try
				{
					stream->Close();
				}
				catch(Exception ^) {}

				try
				{
					delete stream;
				}
				catch (Exception^) {}
			}
		}

		//when running a backup, the remote side needs to confirm back to us
		//if something goes wrong, we hold on the last command so we can abort the backup, keeping
		//the database from truncating the log or clearing the DCM/BCM, etc...
		//this will prevent us from having to reseed a datbase that misses a backup because something went wrong at the last second
		volatile bool BackupConfirmed = false;

		String ^SQLServer = nullptr;
		String ^instanceName = nullptr;
		String ^databaseName = nullptr;
		IClientVirtualDeviceSet2 *vds = NULL;

		Void SqlServerConnection_InfoMessage(Object ^sender, SqlInfoMessageEventArgs ^e)
		{
//#ifdef xDEBUG
//			VDILog("SqlServeronnectionInfoMessage");
//#endif
			for each(Object ^oinfo in e->Errors)
			{
				auto info = (SqlError ^)oinfo;
#if xDEBUG
				VDILog("Class=" + info->Class.ToString() + " Message=" + info->Message);
#endif
				if (info->Class <= 10)
				{
					// xx percent complete
					//Int32::TryParse(e->Message->Substring(0, info->Message->IndexOf(" ")), (int %)this->PercentComplete);
				}
				else
				{
					//if (this->DBBackupRunning && this->DBError == nullptr) this->DBError = gcnew Exception(info->Message);
					//if (this->DBBackupRunning)
					//{
					//	this->DBErrorMessage += info->Message + "\r\n";
					//}
				}
			}
		}

		///Run SQL Command async that will stream to/from the VDI (i.e. backup/restore)
		Void SQLExecuteAsync(Object ^data)
		{
			//connection for the local SQL Server
			SqlConnection conn;
			auto cmds = (array<String ^> ^)data; //String[]

			try
			{
				conn.ConnectionString = "SERVER=" + this->SQLServer + ";DATABASE=" + databaseName + ";TRUSTED_CONNECTION=True;Pooling=False;";
				conn.InfoMessage += gcnew SqlInfoMessageEventHandler(this, &VDIEngine::SqlServerConnection_InfoMessage);
				//conn.FireInfoMessageEventOnUserErrors = true;
				conn.Open();

				//Pre command (i.e. force database in single_user mode to assure the restore always succeeds)
				try
				{
					if (!String::IsNullOrEmpty(cmds[0]))
					{
						SqlCommand cmd(cmds[0], %conn);
						cmd.CommandType = CommandType::Text;
						cmd.CommandTimeout = 300;
						cmd.ExecuteNonQuery();
					}
				}
				catch (Exception ^e)
				{
					//if the SQL error level is over 20, it will cause the SQL conn to disconnect... check for that and reconnect if necessary
					//an unfinished/junk restore database can cause this, and we don't want it to prevent us from restoring a good copy
#ifdef xDEBUG > 1
					VDILog("DB ERROR PRE=" + e->Message + ": " + cmds[0]);
#endif
					if (conn.State == ConnectionState::Closed) conn.Open();
				}

				//run VDI streaming command (backup/restore)
				SqlCommand cmd(cmds[1], %conn);
				cmd.CommandType = CommandType::Text;
				cmd.CommandTimeout = 0;

				//CommandIssued(this, gcnew CommandIssuedEventArgs(data->ToString()));

				//notification that we are running the backup/restore command, so the stream should be ready to start
				this->DBStreamReady = true;
				this->DBBackupRunning = true;
				//conn.FireInfoMessageEventOnUserErrors = false;

#ifdef xDEBUG
				VDILog("ExecuteNonQuery: " + cmds[1]);
#endif
				cmd.ExecuteNonQuery();
			}
			catch (Exception ^ex)
			{
				this->DBError = ex;
#ifdef xDEBUG
				VDILog("DB ERROR: " + ex->Message);
#endif
			}
			finally
			{
				this->DBBackupRunning = false;
#if xDEBUG > 1
				VDILog("Done Execute");
#endif

				//Post command
				if (!String::IsNullOrEmpty(cmds[2]))
				{
					try
					{
#if xDEBUG > 2
						VDILog("Running " + cmds[2]);
#endif
						SqlCommand cmd(cmds[2], %conn);
						cmd.CommandType = CommandType::Text;
						cmd.CommandTimeout = 300;
						cmd.ExecuteNonQuery();
					}
					catch (Exception ^e)
					{
#if xDEBUG > 1
						VDILog("DB ERROR POST=" + e->Message + ": " + cmds[2]);
#endif
					}
				}

				//connection cleanup
				try
				{
					if (conn.State != ConnectionState::Closed) conn.Close();
				}
				catch (Exception ^)
				{
				}

				this->DBThreadRunning = false;
			}
		}

		inline bool WaitForTask(Task ^%t)
		{
			if (t == nullptr) return true; //if there's no task, consider it done
			while (!t->Wait(100))
			{
				//check for aborts every 100ms
				if (DoneOrAbort(false)) return false;
			}
			delete t;
			t = nullptr;
			return true;
		}

		inline bool DoneOrAbort(bool includeConfirmedBackup)
		{
			MemoryBarrier;
			if (!this->DBThreadRunning)
			{
				return true; //done
			}
			MemoryBarrier;
			if (this->StreamAbort)
			{
				return true; //abort
			}
			MemoryBarrier;
			if (includeConfirmedBackup && this->BackupConfirmed)
			{
				return true; //done
			}
			MemoryBarrier;

			return false;
		}

		///This performs the actual transfer between the VDI and .NET stream for the command running SQLExecuteAsync
		Exception ^ProcessVDIStream(Stream ^s)
		{
			if (!this->DBThreadRunning) return nullptr;
			if (this->DBError != nullptr) return (Exception ^)this->DBError;

			IClientVirtualDevice* vd = NULL;
			try
			{
				VDC_Command *cmd;
				DWORD        completionCode;
				DWORD        bytesTransferred;
				HRESULT hr;
				bool supportsVDC_Complete = false;

				//run VDI GetConfiguration() then Open() to get things started
				VDConfig config = { 0 };
				config.deviceCount = 1;
				config.features = VDFeatures::VDF_RequestComplete;
				config.serverTimeOut = (ULONG)this->VDITimeout;

#ifdef xDEBUG
				VDILog("ProcessVDIStream() Begin");
#endif

				if (FAILED(hr = vds->GetConfiguration(60000, &config))) //if we can't get passed this in 60 seconds, then something has gone horribly wrong
				{
					switch (hr)
					{
					case VD_E_ABORT:
						throw gcnew ApplicationException("GetConfiguration was aborted.");
						break;
					case VD_E_TIMEOUT:
						throw gcnew ApplicationException("GetConfiguration timed out.");
						break;
					default:
						throw gcnew ApplicationException(String::Format("An unknown exception was thrown during GetConfiguration.  HRESULT: {0:X}", hr));
						break;
					};
				}

				if (config.features & VDF_CompleteEnabled)
				{
#if xDEBUG > 1
					VDILog("VDC_Complete is Supported!");
#endif
					supportsVDC_Complete = true;
				}

				//open the VDI device we created in the initialization routing
				msclr::interop::marshal_context m;
				auto wVdsName = m.marshal_as<LPCWSTR>(this->VDIDeviceName);
				if (FAILED(hr = vds->OpenDevice(wVdsName, &vd)))
				{
					throw gcnew ApplicationException(String::Format("VDI OpenDevice Failed.  HRESULT: {0:X}", hr));
				}

				this->StreamRunning = true;

#ifdef xDEBUG 
				VDILog("VDI Loop Begin");
#endif
				//this will allow us to check for cancellations every second
				ReadTask = nullptr;
				WriteTask = nullptr;
				this->VDIStream = s;
				bool isBackup;
				isBackup = false;

				while (SUCCEEDED(hr = vd->GetCommand(this->VDITimeout, &cmd)))
				{
#if xDEBUG > 2
					VDILog("COMMAND:" + cmd->commandCode.ToString() + " Bytes:" + cmd->size);
#endif
					auto arr = gcnew array<System::Byte>(cmd->size);
					auto start = DateTime::Now;
					bytesTransferred = 0;
					switch (cmd->commandCode)
					{
					case VDC_Read:

						//loop until we get the requested number of bytes from .net stream, an abort is requested, or the timeout passes
						//We use a task here because wrapped streams (i.e. compression) must read the inner stream synchronously
						//to see if there is more data, but if the inner stream is done, it can hang for the entire duration of the timeout
						//so we want to check for any error during that time and exit, to make it as efficient as possible.

						while (bytesTransferred < cmd->size)
						{
							if (DoneOrAbort(false)) break;

							if (ReadTask == nullptr) ReadTask = s->ReadAsync(arr, bytesTransferred, cmd->size - bytesTransferred);
							if (ReadTask->Wait(1000))
							{
								DWORD len = ReadTask->Result;
								delete ReadTask;
								ReadTask = nullptr;
#if xDEBUG > 2
								VDILog("Read=" + len.ToString());
#endif
								if (len > 0)
								{
									bytesTransferred += len;
									start = DateTime::Now;
								}
							}
							else
							{
								if ((DateTime::Now - start).TotalMilliseconds >= (double)this->VDITimeout)
								{
									break;
								}
							}
						}

#if xDEBUG > 2
						VDILog("Pulled Bytes:" + bytesTransferred.ToString());
#endif
						//move bytes to VDI buffer
						Marshal::Copy(arr, 0, (IntPtr)cmd->buffer, bytesTransferred);
						this->TotalBytesTransferred += bytesTransferred;

						if (bytesTransferred == cmd->size)
						{
							completionCode = ERROR_SUCCESS;
						}
						else
						{
							completionCode = ERROR_HANDLE_EOF;
						}
						break;

					case VDC_Write:
#if xDEBUG > 2
						VDILog("Writing Bytes:" + cmd->size.ToString());
#endif
						isBackup = true;

						//copy cmd->buffer to the .net byte array
						Marshal::Copy((IntPtr)cmd->buffer, arr, 0, cmd->size);

						//make sure previous write is done
						if (!WaitForTask(WriteTask))
						{
							//this should do the trick if we need to stop the VDI...
							completionCode = ERROR_HANDLE_DISK_FULL;
							break;
						}

						//queue bytes for sending
						WriteTask = s->WriteAsync(arr, 0, cmd->size);

#if xDEBUG > 2
						VDILog("Done Writing Bytes");
#endif

						bytesTransferred = cmd->size;

						this->TotalBytesTransferred += bytesTransferred;

						completionCode = ERROR_SUCCESS;
						break;

					case VDC_Flush:
#if xDEBUG > 2
						VDILog("VDC_Flush");
#endif
						//make sure any writes are complete before we call flush to avoid errors
						if (!WaitForTask(WriteTask))
						{
							//this should do the trick if we need to stop the VDI...
							completionCode = ERROR_HANDLE_DISK_FULL;
							break;
						}

						//run flush
						s->Flush();

						//if VDC_Complete is supported, don't fall through to the VDC_Complete case.
						//if VDC_Complete is not supported, VDC_Flush is generally the last command during a backup,
						//so we need to fall through because VDC_Complete won't be explicitly called
						if (supportsVDC_Complete) break;

					case VDC_Complete:
						if (!isBackup)
						{
							//return immediately if doing a restore
							completionCode = ERROR_SUCCESS;
							break;
						}

						//VDC_Flush and then VDC_Complete (if supported) are called after a backup has completed,
						//set StreamComplete to notify that we are done streaming while we wait for confirmation
						this->StreamComplete = true;

						//we need to hold here and wait to receive confirmation from the destination that everything went AOK.
						//some giant databases can take over an hour to confirm, especially if disk IO's are already high
						completionCode = ERROR_HANDLE_DISK_FULL; //this should do the trick if we need to abort the backup...
						//completionCode = VD_E_ABORT; //doesn't work?
						while (true)
						{
							MemoryBarrier;
							if (this->BackupConfirmed)
							{
								//success!
								completionCode = ERROR_SUCCESS;
								break;
							}
							MemoryBarrier;
							if (DoneOrAbort(false)) break;

							//recheck confirmation every 100ms
							Sleep(100);
						}

						break;

					case VDC_ClearError:
						completionCode = ERROR_SUCCESS;
						break;

					default:
#ifdef xDEBUG
						VDILog("Unknown Command = " + cmd->commandCode);
#endif
						completionCode = ERROR_NOT_SUPPORTED;
						break;
					}

					if (FAILED(hr = vd->CompleteCommand(cmd, completionCode, bytesTransferred, 0)))
					{
						throw gcnew ApplicationException(String::Format("vdi->CompleteCommand Failed. HRESULT: {0:X}", hr));
					}
				}

#ifdef xDEBUG
				VDILog(String::Format("VDI Loop Exiting, hr={0:X}", hr));
#endif
				//VDI should send VD_E_CLOSE to exit out, or something has gone wrong
				if (hr != VD_E_CLOSE)
				{
					switch (hr)
					{
					case VD_E_TIMEOUT:
						throw gcnew ApplicationException("VD_E_TIMEOUT.");
						break;
					case VD_E_ABORT:
						throw gcnew ApplicationException("VD_E_ABORT");
						break;
					default:
						throw gcnew ApplicationException(String::Format("vdi->GetCommand Failed. HRESULT: {0:X}", hr));
						break;
					};
				}
			}
			catch (Exception ^e)
			{
#ifdef xDEBUG
				VDILog("ProcessVDIStream() End");
#endif

#ifdef xDEBUG
				String ^m = "VDI Loop Caught Error:" + e->Message;
				while(e->InnerException != nullptr)
				{
					e = e->InnerException;
					m += "\r\n" + e->Message;
				}
				VDILog(m);
#endif
				if (e->Message == "VD_E_ABORT")
				{
					//if the VDI was aborted it happens before DB.Execute completes,
					//so we need to wait a moment until it exits, which will populate the DBError variable, otherwise we won't have any error other than VD_E_ABORT
					while (this->DBBackupRunning) Sleep(100);
				}

				if (this->DBError == nullptr)
				{
					this->DBError = e;
				}
				return (Exception^)this->DBError;
			}
			finally
			{
				this->StreamRunning = false;
				if(vd != NULL) vd->Release();
			}

			//wait for the backup/restore thread to complete, or else the database may not restore properly
			//plus we need to see if there were any critial errors, and DBError will not be filled in until this is done
			auto start = DateTime::Now;
			while (true)
			{
				if (DoneOrAbort(false)) break;

				//recheck every 100ms
				Sleep(100);
				if ((DateTime::Now - start).TotalMilliseconds >= (double)this->VDITimeout) break;
			}

#ifdef xDEBUG
			VDILog("ProcessVDIStream() End");
#endif
			return (Exception ^)this->DBError;
		}

		///Create a New VDI Device
		void Initialize(void)
		{
			HRESULT hr;
			if (FAILED(hr = CoInitializeEx(NULL, COINIT_MULTITHREADED)) && hr != RPC_E_CHANGED_MODE)
			{
				throw gcnew ApplicationException(String::Format("CoInitializeEx Failed HRESULT: {0:X}", hr));
			}

			IClientVirtualDeviceSet2* Ivds = NULL;
			if (FAILED(hr = CoCreateInstance(CLSID_MSSQL_ClientVirtualDeviceSet, NULL, CLSCTX_INPROC_SERVER, IID_IClientVirtualDeviceSet2, (void**)&Ivds)))
			{
				throw gcnew ApplicationException(String::Format("Unable to get an interface to the virtual device set.  Please check to make sure sqlvdi.dll is registered. HRESULT: {0:X}", hr));
			}
			this->vds = Ivds; //must use a local variable to CoCreateInstance, then we can copy it to the class variable, not sure why CoCreateInstance will not take the class variable...

			//create unique VDI devicename using a GUID
			this->VDIDeviceName = System::Guid::NewGuid().ToString()->ToUpper();

			msclr::interop::marshal_context m;
			auto wVdsName = m.marshal_as<LPCWSTR>(this->VDIDeviceName);
			VDConfig config = { 0 };
			config.deviceCount = 1;
			config.features = VDFeatures::VDF_RequestComplete;
			config.serverTimeOut = (ULONG)this->VDITimeout;

			if (String::IsNullOrEmpty(this->instanceName))
			{
				hr = vds->CreateEx(NULL, wVdsName, &config);
			}
			else
			{
				auto wInstanceName = m.marshal_as<LPCWSTR>(this->instanceName);
				hr = vds->CreateEx(wInstanceName, wVdsName, &config);
			}
			if (FAILED(hr))
			{
				throw gcnew ApplicationException(String::Format("Initializing VDI Failed. HRESULT: {0:X}", hr));
			}
		}

		void DBExecuteCancel()
		{
			if (this->DBThread != nullptr)
			{
				if (this->DBThread->ThreadState == ThreadState::Running)
				{
#ifdef xDEBUG 
					VDILog("Abort DBExecute");
#endif
					this->DBThread->Abort();
					this->DBThreadRunning = false;
				}
				this->DBThread = nullptr;
			}
		}

#ifdef xDEBUG
		static void VDILog(String ^s)
		{
			for (int i = 0; i < 10; i++) //retry logging 10 times then give up
			{
				try
				{
					System::IO::File::AppendAllText("C:\\PowerBack\\VDI.log", DateTime::Now.ToString() + ": " + s + "\r\n");
					break;
				}
				catch (Exception ^)
				{
					Sleep(10);
				}
			}
		}
#endif

	public:
		/// Execute Database Command in the Background That We want to Pipe to VDI, i.e. Backup/Restore
		Void DBExecute(String ^preCommand, String ^command, String ^postCommand)
		{
			//execute these commands on a new thread so they do not block
			this->DBExecuteCancel(); //make sure any previous command is cancelled
			this->DBThread = gcnew Thread(gcnew ParameterizedThreadStart(this, &VDIEngine::SQLExecuteAsync));
			this->DBThreadRunning = true;
			this->PercentComplete = 0;
			this->StreamComplete = false;
			this->TotalBytesTransferred = 0L;
			this->DBStreamReady = false;
			this->DBBackupRunning = false;
			this->StreamAbort = false;
			this->DBError = nullptr;
			this->BackupConfirmed = false;
			//this->DBErrorMessage = "";
			this->DBThread->Start(gcnew array<String ^>(3) { preCommand, command, postCommand });

			//before we return, wait for the streaming DB command to be running (or an error occurred)
			auto start = DateTime::Now;
			while (true)
			{
				MemoryBarrier;
				if (this->DBStreamReady == true) break;
				MemoryBarrier;
				if (this->DBError != nullptr) break;
				MemoryBarrier;
				Sleep(100);
				if ((DateTime::Now - start).TotalMilliseconds >= (double)this->VDITimeout) break; //do not let the pre command timeout
			}
#ifdef xDEBUG
			VDILog("!DBExecute has started, stream can begin anytime!");
#endif
		}

		/// Copy Database Command through the VDI interface to/from .NET Stream
		Tasks::Task<Exception ^> ^RunStream(Stream ^stream)
		{
			//create Func delegate for ExecuteDataTransfer Task
			auto d_ProcessVDIStream = gcnew FuncBind<Stream ^, Exception ^>::_delegate(this, &VDIEngine::ProcessVDIStream);
			//wrap Func<Stream, Exception> in FuncBind, which in turn exposes a Func<Exception> that can be passed to Task::Run()
			auto b_ProcessVDIStream = gcnew FuncBind<Stream ^, Exception ^>(d_ProcessVDIStream, stream);
			//now we can call Task::Run() with a compatible signature, why they don't have more overloads is beyond me...
			//or why C++/CLR doesn't have lambda binding yet...
			auto streamTask = Tasks::Task::Run(b_ProcessVDIStream());

			return streamTask;
		}

		/// Stop RunStream midstream
		Void AbortStream()
		{
			if (this->vds != NULL && this->StreamRunning)
			{
#ifdef xDEBUG
				VDILog("VDI->SignalAbort()");
#endif
				this->StreamAbort = true;
				vds->SignalAbort();
			}
			else
			{
				this->DBExecuteCancel();
			}
		}

		/// Mark that a backup was restored/stored successfully
		/// If it wasn't successful, call AbortStream instead.
		Void ConfirmBackup()
		{
			if (this->StreamRunning)
			{
				this->BackupConfirmed = true;
			}
		}
	};
}