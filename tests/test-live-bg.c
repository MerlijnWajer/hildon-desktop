#include <gtk/gtk.h>
#include <gdk/gdkscreen.h>
#include <cairo.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkwindow.h>
#include <string.h>

/* This just attempts to paint itself at FPS fps and outputs the actual fps it
   has managed. You can then run 'xresponse -i' to check 
   how fast hildon-desktop is rendering it, or top to see CPU usage. */

#define FPS 1

/* #undef this to make the test run in non-composited mode */
#define COMPOSITED 1

/* Area's width and height to render */
#define AREAW 800
#define AREAH 480

#ifndef COMPOSITED
static void set_non_compositing (Display *display, Window xwindow)
{
        Atom atom;
        int one = 1;

        atom = XInternAtom (display, "_HILDON_NON_COMPOSITED_WINDOW", False);

        XChangeProperty (display,
                       xwindow,
                       atom,
                       XA_INTEGER, 32, PropModeReplace,
                       (unsigned char *) &one, 1);
}
#endif

static void set_live_bg (Display *display, Window xwindow)
{
        Atom atom;
        int one = 1;

        g_printerr("%s: xwin %x\n", __func__, (unsigned) xwindow);
        atom = XInternAtom (display, "_HILDON_LIVE_DESKTOP_BACKGROUND", False);

        XChangeProperty (display,
                       xwindow,
                       atom,
                       XA_INTEGER, 32, PropModeReplace,
                       (unsigned char *) &one, 1);
}

/* This is called when we need to draw the windows contents */
static gboolean expose(GtkWidget *widget, GdkEventExpose *event, gpointer userdata)
{
    cairo_t *cr = gdk_cairo_create(widget->window);
    static GTimer *timer = 0;
    static int frame = 0;
    static double last_time = 0;
    double this_time;
    int width, height;
    
    if (!timer) timer = g_timer_new();
    this_time = g_timer_elapsed (timer, NULL);

    cairo_set_source_rgb (cr, 1.0, 1.0, 1.0); /* opaque white */

    /* draw the background */
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    //cairo_paint (cr);

    
    gtk_window_get_size(GTK_WINDOW(widget), &width, &height);

    cairo_set_source_rgb(cr, 1, (sin(this_time)+1.0)/2.0, 0.2);
    cairo_rectangle(cr, 0, 0, AREAW, AREAH);
    cairo_fill(cr);
    cairo_stroke(cr);

    cairo_destroy(cr);

    if (event->count == 0)
      {
        frame++;
        if (frame > 100) 
          {	    
	    double ms = ((this_time - last_time)*1000) / frame;
	    printf("Redraw time: %f ms per frame - %f fps\n", ms, 1000/ms);
	    last_time = this_time;
	    frame = 0;
	  }
      }
    return FALSE;
}

static gboolean timeout(GtkWidget *widget) {
  gtk_widget_queue_draw_area(widget, 0, 0, AREAW, AREAH);
  return TRUE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Live background test");
    g_signal_connect(G_OBJECT(window), "delete-event", gtk_main_quit, NULL);

    gtk_widget_set_app_paintable(window, TRUE);
    //gtk_widget_set_double_buffered(window, FALSE);

    g_signal_connect(G_OBJECT(window), "expose-event", G_CALLBACK(expose), NULL);

    /* toggle title bar on click - we add the mask to tell X we are interested
     * in this event */
    //gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    g_timeout_add(1000/FPS, (GSourceFunc)timeout, window);

    gtk_widget_realize (window);

    Display *display = XOpenDisplay (NULL);

#ifndef COMPOSITED
    set_non_compositing (display, GDK_WINDOW_XID (GTK_WIDGET (window)->window));
#endif

    gtk_window_fullscreen (GTK_WINDOW (window));

    gtk_widget_show_all(window);
    set_live_bg (display, GDK_WINDOW_XID (GTK_WIDGET (window)->window));

    gtk_main();

    return 0;
}


