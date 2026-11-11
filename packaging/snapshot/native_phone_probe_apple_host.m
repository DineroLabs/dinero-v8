#import <UIKit/UIKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

extern int nodecore_qualification_main(int argc, char **argv);
static int probe_argc;
static char **probe_argv;
static BOOL probe_started=NO;

@interface ProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@property(nonatomic,strong) UILabel *status;
@end

@interface ProbeDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation ProbeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application;(void)options;return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application;(void)options;
    UISceneConfiguration *configuration=[[UISceneConfiguration alloc] initWithName:@"Qualification" sessionRole:session.role];
    configuration.delegateClass=ProbeSceneDelegate.class;
    configuration.sceneClass=UIWindowScene.class;
    return configuration;
}
@end

@implementation ProbeSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session;(void)options;
    if(![scene isKindOfClass:UIWindowScene.class])return;
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *controller=[UIViewController new];
    controller.view.backgroundColor=UIColor.systemBackgroundColor;
    self.status=[[UILabel alloc] initWithFrame:CGRectInset(self.window.bounds,24,80)];
    self.status.numberOfLines=0;
    self.status.textAlignment=NSTextAlignmentCenter;
    self.status.text=@"Snapshot qualification running\nKeep this screen open.";
    [controller.view addSubview:self.status];
    self.window.rootViewController=controller;
    [self.window makeKeyAndVisible];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    UIApplication *application=UIApplication.sharedApplication;
    application.idleTimerDisabled=YES;
    if(probe_started)return;
    probe_started=YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        int result=nodecore_qualification_main(probe_argc,probe_argv);
        fflush(NULL);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.status.text=[NSString stringWithFormat:@"Native probe exited: %d",result];
            application.idleTimerDisabled=NO;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(), ^{ exit(result); });
        });
    });
}
- (void)sceneWillResignActive:(UIScene *)scene {(void)scene;UIApplication.sharedApplication.idleTimerDisabled=NO;}
@end

int main(int argc,char **argv) {
    probe_argc=argc;probe_argv=argv;
    @autoreleasepool {
        // Retain the complete, unfiltered console in this isolated app's data
        // container. The console transport may disconnect during a long mainnet
        // audit; a transport exit is never a native qualification verdict.
        if(argc>2 && (!strcmp(argv[1],"run") || !strcmp(argv[1],"call"))) {
            NSString *leaf=[[NSString stringWithUTF8String:argv[2]] lastPathComponent];
            NSString *name=[NSString stringWithFormat:@"%@.%d.console.log",leaf,getpid()];
            NSString *path=[[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"] stringByAppendingPathComponent:name];
            int fd=open(path.fileSystemRepresentation,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if(fd<0){perror("qualification log open");return 2;}
            printf("NCPROBE_HOST_LOG\t%s\n",path.fileSystemRepresentation);fflush(stdout);
            if(dup2(fd,STDOUT_FILENO)<0 || dup2(fd,STDERR_FILENO)<0){close(fd);return 2;}
            close(fd);
        }
        return UIApplicationMain(argc,argv,nil,NSStringFromClass(ProbeDelegate.class));
    }
}
