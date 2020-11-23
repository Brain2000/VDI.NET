# VDI.NET
Microsoft SQL VDI (Virtual Device Interface) that bridges to a .NET stream

# Usage Flow:
1) Create a VDIEngine instance (henceforth known as "vdi").

2) Call vdi.DBExecute() and run a SQL command for either a BACKUP or RESTORE.
   The VDI device name is vdi.VDIDeviceName for the VIRTUAL_DEVICE, which is used instead of the DISK parameter.

3) Check vdi.DBError for any immediate errors (i.e. database does not exist)

4) Start the backup/restore by calling vdi.RunStream(), which will return a Task.

5) Loop and check for any possible errors, as well as Task.IsComplete or vdi.StreamComplete.
   a) If an error occurs (i.e. pushing to remote system and it runs out of drive space) call vdi.AbortStream() and wait for the Task to complete.
   b) If running a BACKUP and vdi.StreamComplete is true, verify that the data has completed (i.e. if restoring the backup stream to another server, verify that the remote restore completes) and then call either vdi.ConfirmBackup() or vdi.AbortStream().
   c) Once Task.IsComplete is true, you may exit the loop.

# Notes:
1) The process running the VDI must be on the same server as the SQL server, as VDI uses COM.

2) The VDI uses a trusted connection when running the BACKUP/RESTORE commands.

3) Network streams cannot be cancelled, so if you need to dispose of a VDI while in the middle of a long network timeout, you can signal the VDI to close your stream for you after the timeout completes.
   Set vdi.DisposeVDIStreamAfterReadWriteTask = true before calling vdi.Dispose().
   Note: on long timeouts for gigantic databases, this can be an hour or more!

4) A RESTORE usually must be run from the master database context to avoid locking issues, so when instantiating the VDIEngine( ), use "master" as the initial database parameter.

# Example:
```
Stream outputStream; //this can attach to anything that accepts a stream... network socket... filesystem...

var initialDatabase = "MyDatabase"; //use master when restoring
var vdi = new VDIDotNet.VDIEngine("localhost\\INSTANCE", initialDatabase, msTimeout);

//note, you would never want to set a database to single user mode for a backup, but this is just an example of PRE/POST command usage
var SQLPreCmd = "ALTER DATABASE [MyDatabase] SET ONLINE,SINGLE_USER WITH ROLLBACK IMMEDIATE";
var SQLBackupCmd = BACKUP DATABASE [MyDatabase] TO VIRTUAL_DEVICE='" + vdi.VDIDeviceName + "' WITH CHECKSUM";
var SQLPostCmd = "ALTER DATABASE [MyDatabase] SET ONLINE,MULTI_USER";

vdi.DBExecute(SQLPreCmd, SQLBackupCmd, SQLPostCmd);
Threading.Tasks.Task<Exception> task;
if(vdi.DBError != null)
{
    task = Threading.Tasks.Task.FromResult(vdi.DBError);
}
else
{
    task = vdi.RunStream(outputStream);
}

while(!task.IsComplete)
{
    if (somethingWentWrong)
    {
        vdi.AbortStream();

        var tasks = new Threading.Tasks.Task[1];
        tasks[0] = task;
        if (Threading.Tasks.Task.WaitAny(tasks, msTimeout) == -1) //wait for VDI to finish, WaitAny() will not throw task exceptions
        {
            //error, VDI not stopping
            break;
        }
    }

    if (vdi.StreamComplete)
    {
        vdi.ConfirmBackup();

        if (!task.Wait(msTimeout)) //wait for VDI to finish, task.Wait( ) will throw task exceptions
        {
            //error, VDI not stopping
            break;
        }
    }
}
Exception BackupRestoreError = task.Result;

task.Dispose();
vdi.Dispose();
```
