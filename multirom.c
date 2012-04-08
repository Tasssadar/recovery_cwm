#include <time.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/input.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "multirom.h"
#include "extendedcommands.h"
#include "minui/minui.h"
#include "recovery_ui.h"
#include "minzip/SysUtil.h"
#include "minzip/Zip.h"
#include "install.h"
#include "roots.h"

#define UPDATE_SCRIPT_PATH  "META-INF/com/google/android/"
#define UPDATE_SCRIPT_NAME  "META-INF/com/google/android/updater-script"

void multirom_deactivate_backup(unsigned char copy)
{
    // Just to make sure it exists
    __system("mkdir /sd-ext/multirom/backup");

    char cmd[512];
    time_t t = time(0);
    struct tm * loctm = localtime(&t);

    sprintf(cmd, "%s /sd-ext/multirom/rom /sd-ext/multirom/backup/rom_%u%02u%02u-%02u%02u && sync", copy ? "cp -r -p" : "mv",
            loctm->tm_year+1900, loctm->tm_mon+1, loctm->tm_mday, loctm->tm_hour, loctm->tm_min);

    if(copy)
        ui_print("Create backup?\n");
    else
        ui_print("Deactivate ROM?\n");
    ui_print("Menu to confirm, any other key\nto go back\n");
    int key = ui_wait_key();
    if(key != KEY_MENU)
        return;
    ui_print("Working...\n");
    pid_t pid = fork();
    if (pid == 0)
    {
        char *args[] = { "/sbin/sh", "-c", cmd, "1>&2", NULL };
        execv("/sbin/sh", args);
        _exit(-1);
    }

    int status;
    while (waitpid(pid, &status, WNOHANG) == 0)
    {
        ui_print(".");
        sleep(1);
    }
    ui_print("\n");

    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
    {
        ui_print("Unable to complete!\n");
        return;
    }

}

void multirom_activate_backup(char *path, unsigned char copy)
{
    char cmd[256];
    sprintf(cmd, "%s \"%s\" /sd-ext/multirom/rom && sync", copy ? "cp -r -p" : "mv", path);

    ui_print("\nActivate backup?");
    ui_print("Menu to confirm, any other key\nto go back\n");
    int key = ui_wait_key();
    if(key != KEY_MENU)
        return;
    ui_print("Working...\n");
    pid_t pid = fork();
    if (pid == 0)
    {
        char *args[] = { "/sbin/sh", "-c", cmd, "1>&2", NULL };
        execv("/sbin/sh", args);
        _exit(-1);
    }

    int status;
    while (waitpid(pid, &status, WNOHANG) == 0)
    {
        ui_print(".");
        sleep(1);
    }
    ui_print("\n");

    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
    {
        ui_print("Unable to activate!\n");
        return;
    }

    sync();
}

char *multirom_list_backups(void)
{
    DIR *dir = opendir("/sd-ext/multirom/backup");
    if(!dir)
    {
        ui_print("Could not open backup dir, no backups present?\n");
        return NULL;
    }

    struct dirent * de = NULL;
    unsigned char total = 0;
    char *backups[MULTIROM_BACKUPS_MAX];

    while ((de = readdir(dir)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;
        backups[total] = (char*)malloc(128);
        strcpy(backups[total++], de->d_name);
        if(total >= MULTIROM_BACKUPS_MAX-1)
            break;
    }
    backups[total] = NULL;

    if(total == 0)
    {
        ui_print("No backups found");
        return NULL;
    }
    closedir(dir);

    static char* headers[] = { "Choose backup",
                               "or press BACK to return",
                               "",
                               NULL };

    for (;;) {
        int chosen_item = get_menu_selection(headers, backups, 0, 0);
        if (chosen_item >= 0)
        {
            ui_end_menu();
            char *path = (char*)malloc(128);
            ui_print("Selected backup: %s\n", backups[chosen_item]);
            sprintf(path, "/sd-ext/multirom/backup/%s", backups[chosen_item]);

            // Free backup names from memory
            unsigned char i = 0;
            for(; i < total; ++i)
                free(backups[i]);
            return path;
        }else return NULL;
    }
    return NULL;
}

char multirom_exract_ramdisk(void)
{
    ui_print("Dumping boot img...\n");
    if(__system("dump_image boot /tmp/boot.img") != 0)
    {
        ui_print("Could not dump boot.img!\n");
        return -1;
    }

    FILE *boot_img = fopen("/tmp/boot.img", "r");
    FILE *ramdisk = fopen("/tmp/rd.cpio.gz", "w");
    if(!boot_img || !ramdisk)
    {
        ui_print("Could not open boot.img or ramdisk!\n");
        return -1;
    }

    // load needed ints
    struct boot_img_hdr header;
    unsigned *start = &header.kernel_size;
    fseek(boot_img, BOOT_MAGIC_SIZE, SEEK_SET);
    fread(start, 4, 8, boot_img);

    // get ramdisk offset
    unsigned int ramdisk_pos = (1 + ((header.kernel_size + header.page_size - 1) / header.page_size))*2048;
    ui_print("Ramdisk addr: %u\nRamdisk size: %u\n", ramdisk_pos, header.ramdisk_size);

    // get ramdisk!
    char *buffer = (char*) malloc(header.ramdisk_size);
    fseek(boot_img, ramdisk_pos, SEEK_SET);
    fread(buffer, 1, header.ramdisk_size, boot_img);
    fwrite(buffer, 1, header.ramdisk_size, ramdisk);
    fflush(ramdisk);
    fclose(boot_img);
    fclose(ramdisk);
    free(buffer);

    // extact it...
    ui_print("Extracting init files...\n");
    if(__system("mkdir -p /tmp/boot && cd /tmp/boot && gzip -d -c /tmp/rd.cpio.gz | busybox cpio -i") != 0)
    {
        __system("rm -r /tmp/boot");
        ui_print("Failed to extract boot image!\n");
        return -1;
    }

    // copy our files
    __system("mkdir /sd-ext/multirom/rom/boot");
    __system("cp /tmp/boot/*.rc /sd-ext/multirom/rom/boot/");
    __system("cp /tmp/boot/default.prop /sd-ext/multirom/rom/boot/default.prop");
    __system("cp /tmp/boot/sbin/adbd /sd-ext/multirom/rom/boot/");

    FILE *init_f = fopen("/tmp/boot/main_init", "r");
    if(init_f)
    {
        fclose(init_f);
        __system("cp /tmp/boot/main_init /sd-ext/multirom/rom/boot/init");
    }
    else __system("cp /tmp/boot/init /sd-ext/multirom/rom/boot/init");

    __system("rm /sd-ext/multirom/rom/boot/preinit.rc");

    sync();

    // and delete temp files
    __system("rm -r /tmp/boot");
    __system("rm /tmp/boot.img");
    __system("rm /tmp/rd.cpio.gz");
    return 0;
}

char multirom_copy_folder(char *folder)
{
    ui_print("Copying folder /%s", folder);

    char cmd[100];
    sprintf(cmd, "mkdir /sd-ext/multirom/rom/%s", folder);
    __system(cmd);

    sprintf(cmd, "cp -r -p /%s/* /sd-ext/multirom/rom/%s/ && sync", folder, folder);
    pid_t pid = fork();
    if (pid == 0)
    {
        char *args[] = { "/sbin/sh", "-c", cmd, "1>&2", NULL };
        execv("/sbin/sh", args);
        _exit(-1);
    }

    int status;
    while (waitpid(pid, &status, WNOHANG) == 0)
    {
        ui_print(".");
        sleep(1);
    }
    ui_print("\n");

    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
    {
        ui_print("Failed to copy /%s!\n", folder);
        return -1;
    }

    sync();

    return 0;
}

void multirom_change_mountpoints(char apply)
{
    ensure_path_unmounted("/system");
    ensure_path_unmounted("/data");

    if(apply)
    {
        __system("cp /etc/recovery.fstab /etc/recovery.fstab.back");
        __system("cp /etc/fstab /etc/fstab.back");
        __system("echo \"/data\tauto\t/sd-ext/multirom/rom/data\t/inv\tauto\tbind\tbind\" >> /etc/recovery.fstab");
        __system("echo \"/system\tauto\t/sd-ext/multirom/rom/system\t/inv\tauto\tbind\tbind\" >> /etc/recovery.fstab");
        __system("echo \"/sd-ext/multirom/rom/system /system auto bind\" >> /etc/fstab");
        __system("echo \"/sd-ext/multirom/rom/data /data auto bind\" >> /etc/fstab");
    }
    else
    {
        __system("cp /etc/recovery.fstab.back /etc/recovery.fstab");
        __system("cp /etc/fstab.back /etc/fstab");
    }


    free(get_device_volumes());
    load_volume_table();
}

char multirom_backup_boot_image(char restore)
{
    if(restore)
    {
        ui_print("Restoring boot.img...\n");
        if(__system("flash_image boot /tmp/boot-backup.img") != 0)
        {
            ui_print("Could not restore boot.img!\n");
            return -1;
        }
    }
    else
    {
        ui_print("Creating backup of boot.img...\n");
        __system("rm /tmp/boot-backup.img");
        if(__system("dump_image boot /tmp/boot-backup.img") != 0)
        {
            ui_print("Could not dump boot.img!\n");
            return -1;
        }
    }
    return 0;
}

int multirom_prepare_zip_file(char *file)
{
    int ret = 0;

    char cmd[256];
    __system("rm /tmp/mr_update.zip");
    sprintf(cmd, "cp \"%s\" /tmp/mr_update.zip", file);
    __system(cmd);

    sprintf(cmd, "mkdir -p /tmp/%s", UPDATE_SCRIPT_PATH);
    __system(cmd);

    sprintf(cmd, "/tmp/%s", UPDATE_SCRIPT_NAME);

    FILE *new_script = fopen(cmd, "w");
    if(!new_script)
        return -1;

    ZipArchive zip;
    int err = mzOpenZipArchive("/tmp/mr_update.zip", &zip);
    if (err != 0)
    {
        ret = err;
        goto exit;
    }

    const ZipEntry *script_entry = mzFindZipEntry(&zip, UPDATE_SCRIPT_NAME);
    if(!script_entry)
    {
        ret = -1;
        goto exit;
    }

    int script_len;
    char* script_data;
    if (read_data(&zip, script_entry, &script_data, &script_len) < 0)
    {
        ret = -1;
        goto exit;
    }

    mzCloseZipArchive(&zip);

    int itr = 0;
    char *token = strtok(script_data, "\n");
    while(token)
    {
        if((!strstr(token, "mount") && !strstr(token, "format")) || !strstr(token, "MTD"))
        {
            fputs(token, new_script);
            fputc('\n', new_script);
        }
        token = strtok(NULL, "\n");
    }

    free(script_data);
    fclose(new_script);

    sprintf(cmd, "cd /tmp && zip mr_update.zip %s", UPDATE_SCRIPT_NAME);
    if(__system(cmd) < 0)
        return -1;

goto exit_succes;

exit:
    mzCloseZipArchive(&zip);
    fclose(new_script);

exit_succes:
    return ret;
}

void multirom_flash_zip(char *file, char newRom)
{
    ui_print("Preparing ZIP file...\n");
    if(multirom_prepare_zip_file(file) < 0)
    {
        ui_print("Сan't prepare ZIP file!\n");
        return;
    }

    if(multirom_backup_boot_image(0) != 0)
        return;

    ui_print("Mounting folders...\n");

    __system("mkdir -p /sd-ext/multirom/rom/system");
    __system("mkdir -p /sd-ext/multirom/rom/cache");
    __system("mkdir -p /sd-ext/multirom/rom/data");
    __system("mkdir -p /sd-ext/multirom/rom/boot");

    multirom_change_mountpoints(1);
    ensure_path_mounted("/system");
    ensure_path_mounted("/data");

    ui_print("Executing update package...\n");

    int status = install_package("/tmp/mr_update.zip");

    __system("rm /tmp/mr_update.zip");

    if (status != INSTALL_SUCCESS)
    {
        if(newRom)
            __system("rm -r /sd-ext/multirom/rom");

        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("\nInstallation aborted.\n");

        ui_print("Restoring mount points\n");
        multirom_change_mountpoints(0);
        return;
    }
    else
        ui_print("\nInstall from sdcard complete.\n");

    if(newRom)
    {
        ui_print("Extracting boot image...\n");
        multirom_exract_ramdisk();
    }

    multirom_backup_boot_image(1);

    ui_print("Restoring mount points\n");
    multirom_change_mountpoints(0);

    ui_print("Copying modules...\n");
    ensure_path_mounted("/system");
    __system("cp /system/lib/modules/* /sd-ext/multirom/rom/system/lib/modules/ && sync");

    ui_print("Complete!\n");
}
