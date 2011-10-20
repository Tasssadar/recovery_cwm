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
    sprintf(cmd, "%s %s /sd-ext/multirom/rom && sync", copy ? "cp -r -p" : "mv", path);

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

char *multirom_list_backups()
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

char multirom_exract_ramdisk()
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
